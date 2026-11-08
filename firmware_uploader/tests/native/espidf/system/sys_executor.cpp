#include "sys_common.h"
int main(int argc, char **argv) {
  SysNode n;
  Args a(argc, argv);
  { std::istringstream ss(a.get("consumers")); std::string item; while (std::getline(ss, item, ',')) { size_t c = item.find(':'); if (c != std::string::npos) n.pins.push_back(atoi(item.substr(c + 1).c_str())); } }
  return sysMain(argc, argv, n);
}
