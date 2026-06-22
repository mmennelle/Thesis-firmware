import json, sys
path = "/etc/cs-reader/192.168.8.212.json"
with open(path) as f:
    data = json.load(f)
data["username"] = "cs_reader"
data["password"] = "A0CcljzbSZ4rOfku2WOvEXT37kdRj10v"
with open(path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
print("patched", path, "username=cs_reader")
