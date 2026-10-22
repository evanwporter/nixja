#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> messages = {"Hello", "from", "C++"};

    for (const auto& msg : messages) {
        std::cout << msg << " ";
    }
    std::cout << std::endl;

    return 0;
}
