#include <iostream>
#include <string>

int main()
{
	std::cout << "UEXPLORER_INJECTION_TARGET_READY" << std::endl;
	std::string command;
	if (!std::getline(std::cin, command))
		return 1;
	return command == "exit" ? 0 : 2;
}
