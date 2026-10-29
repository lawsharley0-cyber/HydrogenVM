with open("src/runtime/Process.hpp", "r") as f:
    hpp = f.read()

if "void registerLibcxx(Process& p);" not in hpp:
    hpp = hpp.replace("void registerLibStdcxx(Process& p);", "void registerLibStdcxx(Process& p);\nvoid registerLibcxx(Process& p);")
    with open("src/runtime/Process.hpp", "w") as f:
        f.write(hpp)
        print("Updated Process.hpp")
else:
    print("Process.hpp already updated")
