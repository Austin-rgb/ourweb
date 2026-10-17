#include <ourweb/app.hpp>

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ourweb;

struct User {
    int id = 0;
    std::string name;
    std::string email;
};

// nlohmann/json support
void to_json(nlohmann::json& j, const User& u) {
    j = nlohmann::json{{"id", u.id}, {"name", u.name}, {"email", u.email}};
}

struct CreateUser {
    std::string name;
    std::string email;
};

void from_json(const nlohmann::json& j, CreateUser& u) {
    j.at("name").get_to(u.name);
    j.at("email").get_to(u.email);
}

// Shared mutable state: handlers run concurrently, so guard it with a mutex.
struct Users {
    std::mutex mu;
    std::unordered_map<int, User> by_id;
    int next_id = 1;
};

int main() {
    App app;
    app.use_logger();

    auto users = std::make_shared<Users>();
    users->by_id[1] = User{1, "Alice", "alice@example.com"};
    users->by_id[2] = User{2, "Bob", "bob@example.com"};
    users->next_id = 3;

    // GET /users
    app.get("/users", [users] {
        std::lock_guard<std::mutex> lock(users->mu);
        std::vector<User> all;
        for (const auto& [id, user] : users->by_id) all.push_back(user);
        return json(all);
    });

    // GET /users/{id}   -- Path<int> is parsed for you; "/users/abc" is a 400
    app.get("/users/{id}", [users](Path<int> id) {
        std::lock_guard<std::mutex> lock(users->mu);
        auto it = users->by_id.find(*id);
        if (it == users->by_id.end()) return not_found("User not found");
        return json(it->second);
    });

    // POST /users       -- Json<CreateUser> is parsed for you; bad JSON is a 400,
    //                      a wrong Content-Type is a 415
    app.post("/users", [users](Json<CreateUser> input) {
        std::lock_guard<std::mutex> lock(users->mu);
        User user{users->next_id++, input->name, input->email};
        users->by_id[user.id] = user;
        return json(user, HttpStatus::Created)
            .header("Location", "/users/" + std::to_string(user.id));
    });

    // PUT /users/{id}   -- path and body together
    app.put("/users/{id}", [users](Path<int> id, Json<CreateUser> input) {
        std::lock_guard<std::mutex> lock(users->mu);
        auto it = users->by_id.find(*id);
        if (it == users->by_id.end()) return not_found("User not found");
        it->second.name = input->name;
        it->second.email = input->email;
        return json(it->second);
    });

    // DELETE /users/{id}
    app.del("/users/{id}", [users](Path<int> id) {
        std::lock_guard<std::mutex> lock(users->mu);
        return users->by_id.erase(*id) ? no_content() : not_found("User not found");
    });

    std::cout << "Starting OurWeb users_api on 127.0.0.1:8080\n";
    app.run("127.0.0.1:8080");
    return 0;
}
