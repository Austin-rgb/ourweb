#include <ourweb/app.hpp>

#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>

struct User {
    int id;
    std::string name;
    std::string email;
};

// nlohmann/json support
void to_json(nlohmann::json& j, const User& u) {
    j = nlohmann::json{{"id", u.id}, {"name", u.name}, {"email", u.email}};
}

void from_json(const nlohmann::json& j, User& u) {
    j.at("name").get_to(u.name);
    j.at("email").get_to(u.email);
    // id is assigned by the server
}

struct AppState {
    std::mutex mu;
    std::unordered_map<int, User> users;
    int next_id = 1;
};

int main() {
    ourweb::App app;

    app.state(std::make_shared<AppState>());
    app.use_logger();

    // GET /users
    app.get("/users", [](ourweb::Request /*req*/) {
        // For demo we return a static list; real state access would use State<>
        return ourweb::json(std::vector<User>{
            {1, "Alice", "alice@example.com"},
            {2, "Bob", "bob@example.com"}
        });
    });

    // GET /users/{id}
    app.get("/users/{id}", [](ourweb::Request req) {
        auto id_str = req.path_param("id");
        if (!id_str) {
            return ourweb::bad_request("Missing id");
        }
        auto id_opt = ourweb::detail::path_converter<int>::convert(*id_str);
        if (!id_opt) {
            return ourweb::invalid_parameter("id must be an integer");
        }
        int id = *id_opt;
        // Demo: only id 1 and 2 exist
        if (id == 1) {
            return ourweb::json(User{1, "Alice", "alice@example.com"});
        }
        if (id == 2) {
            return ourweb::json(User{2, "Bob", "bob@example.com"});
        }
        return ourweb::not_found("User not found");
    });

    // POST /users
    app.post("/users", [](ourweb::Request req) {
        auto parsed = ourweb::parse_json<User>(req);
        if (!parsed) {
            return ourweb::invalid_json();
        }
        User u = **parsed;
        u.id = 42; // demo
        return ourweb::json(u, ourweb::HttpStatus::Created);
    });

    std::cout << "Starting OurWeb users_api on 127.0.0.1:8080\n";
    app.run("127.0.0.1:8080");
    return 0;
}
