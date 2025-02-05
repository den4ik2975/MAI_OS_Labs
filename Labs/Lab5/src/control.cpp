#include "unordered_set"
#include "zmq_operations.h"

class Controller {
private:
    std::unordered_set<int> node_ids_;
    std::list<Message> pending_messages_;
    std::list<Node> children_;
    std::chrono::milliseconds heartbeat = std::chrono::milliseconds::zero();
    std::map<int, std::chrono::system_clock::time_point> beat_tracker;

    void handle_child_message(const Message& message) {
        switch (message.command) {
            case CommandType::Create:
                node_ids_.insert(message.id);
                beat_tracker[message.id] = now();
                std::cout << "Ok: " << message.add_data << std::endl;
                remove_pending_message(CommandType::Create, message.id);
                break;

            case CommandType::Ping:
                std::cout << "Ok: " << message.id << " is available" << std::endl;
                remove_pending_message(CommandType::Ping, message.id);
                break;

            case CommandType::ExecErr:
                std::cout << "Ok: " << message.id << " '" << message.val << "' not found" << std::endl;
                remove_pending_message(CommandType::ExecFnd, message.id);
                break;

            case CommandType::ExecAdd:
                std::cout << "Ok: " << message.id << std::endl;
                remove_pending_message(CommandType::ExecAdd, message.id);
                break;

            case CommandType::ExecFnd:
                std::cout << "Ok: " << message.id << " '" << message.val << "' " << message.add_data << std::endl;
                remove_pending_message(CommandType::ExecFnd, message.id);
                break;
            case CommandType::HeartBeat:
                std::cout << "Ok: " << message.id << " Got beat" << std::endl;
                beat_tracker[message.id] = now();
        }
    }

    void check_pending_messages() {
        auto now_time = now();
        auto it = pending_messages_.begin();
        while (it != pending_messages_.end()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now_time - it->sent_time).count() > 5) {
                handle_timeout(*it);
                it = pending_messages_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void handle_timeout(const Message& message) {
        switch (message.command) {
            case CommandType::Ping:
                std::cout << "Error: Ok " << message.id << " is unavailable" << std::endl;
                break;
            case CommandType::Create:
                std::cout << "Error: Parent " << message.id << " is unavailable" << std::endl;
                break;
            case CommandType::ExecAdd:
            case CommandType::ExecFnd:
                std::cout << "Error: Node " << message.id << " is unavailable" << std::endl;
                break;
        }
    }

    void remove_pending_message(CommandType cmd, int id) {
        auto it = std::find_if(pending_messages_.begin(), pending_messages_.end(),
            [cmd, id](const Message& m) {
                return (m.command == cmd && m.id == id) ||
                    (m.command == cmd && m.command == CommandType::Create && m.add_data == id);
            });
        if (it != pending_messages_.end()) {
            pending_messages_.erase(it);
        }
    }

    void broadcast_message(const Message& message) {
        pending_messages_.push_back(message);
        for (auto& child : children_) {
            zmq_lib::send_message(child, message);
        }
    }

    void handle_create_command() {
        int parent_id, child_id;
        std::cin >> child_id >> parent_id;

        if (node_ids_.count(child_id)) {
            std::cout << "Error: Node with id " << child_id << " already exists" << std::endl;
            return;
        }

        if (!node_ids_.count(parent_id)) {
            std::cout << "Error: Parent with id " << parent_id << " not found" << std::endl;
            return;
        }

        if (parent_id == -1) {
            Node child = create_process(child_id);
            children_.push_back(std::move(child));
            node_ids_.insert(child_id);
            std::cout << "Ok: " << child.pid << std::endl;
        } else {
            broadcast_message(Message(CommandType::Create, parent_id, child_id));
        }
    }

    void handle_exec_command() {
        char input[100];
        fgets(input, sizeof(input), stdin);

        int id, val;
        char key[30];

        if (sscanf(input, "%d %30s %d", &id, key, &val) == 3) {
            if (!node_ids_.count(id)) {
                std::cout << "Error: Node with id " << id << " doesn't exist" << std::endl;
                return;
            }
            broadcast_message(Message(CommandType::ExecAdd, id, val, key));
        }
        else if (sscanf(input, "%d %30s", &id, key) == 2) {
            if (!node_ids_.count(id)) {
                std::cout << "Error: Node with id " << id << " doesn't exist" << std::endl;
                return;
            }
            broadcast_message(Message(CommandType::ExecFnd, id, -1, key));
        }
    }

    void handle_ping_command() {
        int id;
        std::cin >> id;
        if (!node_ids_.count(id)) {
            std::cout << "Error: Node with id " << id << " doesn't exist" << std::endl;
        } else {
            broadcast_message(Message(CommandType::Ping, id, 0));
        }
    }

    void handle_heartbeat_command()
    {
        int time;
        std::cin >> time;
        heartbeat = std::chrono::milliseconds(time);
        for (auto& [key, value] : beat_tracker)
        {
            value = now();
        }

        auto msg = Message(CommandType::HeartBeat, -1, time);
        for (auto& child : children_) {
            zmq_lib::send_message(child, msg);
        }
    }

    void check_beats() {
        for (auto& [key, value] : beat_tracker) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - value).count() > 4 * heartbeat.count())
            {
                std::cout << "Node: " << key << " has no beat!" << std::endl;
                value = now();
            }
        }
    }

public:
    Controller() {
        node_ids_.insert(-1);
    }

    void run() {
        std::string command;
        while (true) {
            // Handle messages from children
            for (auto& child : children_) {
                Message message = zmq_lib::receive_message(child);
                if (message.command != CommandType::None) {
                    handle_child_message(message);
                }
            }

            check_pending_messages();

            if (heartbeat > std::chrono::milliseconds::zero()) {
                check_beats();
            }

            if (!input_available()) {
                continue;
            }

            std::cin >> command;
            if (command == "create") {
                handle_create_command();
            }
            else if (command == "exec") {
                handle_exec_command();
            }
            else if (command == "ping") {
                handle_ping_command();
            }
            else if (command == "heartbeat")
            {
                handle_heartbeat_command();
            }
            else {
                std::cout << "Error: Command doesn't exist!" << std::endl;
            }
        }
    }
};

int main() {
    try {
        Controller controller;
        controller.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
