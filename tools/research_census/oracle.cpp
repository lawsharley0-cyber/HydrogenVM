// Ground-truth oracle: constructs a HydrogenVM Process (no guest code is loaded or run)
// and asks which symbols/classes/selectors the runtime registers.
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#include <iostream>
#include <fstream>
#include <vector>
using namespace Hydrogen::Runtime;
int main(int argc,char**argv){
  ProcessConfig cfg; cfg.bundlePath=argv[1]; cfg.homePath=argv[2]; cfg.log=[](LogLevel,const std::string&){};
  Process p(cfg);
  auto& r=ObjC::rt(p);
  // stdin lines: "S name" | "C name" | "M selector" ; classes list from argv[3] file
  std::vector<std::string> classes; { std::ifstream f(argv[3]); std::string l; while(std::getline(f,l)) classes.push_back(l);}  
  std::string kind,name;
  while(std::cin>>kind>>name){
    if(kind=="S") std::cout<<"S "<<name<<' '<<(p.lookupSymbol(name).has_value()?1:0)<<"\n";
    else if(kind=="C"){ auto c=r.getClass(name); auto* ci=c?r.info(c):nullptr; std::cout<<"C "<<name<<' '<<(c?(ci&&ci->stub?2:1):0)<<"\n"; }
    else { int ok=0; auto s=r.sel(name); for(auto&cn:classes){ auto c=r.getClass(cn); if(!c) continue; auto* ci=r.info(c); if(ci&&ci->stub) continue; if(r.lookup(c,s)||r.lookup(r.classOf(c),s)){ok=1;break;} }
      std::cout<<"M "<<name<<' '<<ok<<"\n"; }
  }
}
