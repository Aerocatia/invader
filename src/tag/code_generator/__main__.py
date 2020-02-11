# SPDX-License-Identifier: GPL-3.0-only

import sys
import json
import os

from definition import make_definitions
from parser import make_parser

if len(sys.argv) < 12:
    print("Usage: {} <definition.hpp> <parser.hpp> <parser-save-hek-data.cpp> <parser-read-hek-data.cpp> <parser-read-cache-file-data.cpp> <parser-cache-format.cpp> <parser-cache-deformat.cpp> <parser-refactor-reference.cpp> <enum.cpp> <extract-hidden> <json> [json [...]]".format(sys.argv[0]), file=sys.stderr)
    sys.exit(1)

files = []
all_enums = []
all_bitfields = []
all_structs = []

extract_hidden = True if sys.argv[9].lower() == "on" else False

for i in range(11, len(sys.argv)):
    def make_name_fun(name, ignore_numbers):
        name = name.replace(" ", "_").replace("'", "").replace("-","_")
        if not ignore_numbers and name[0].isnumeric():
            name = "_{}".format(name)
        return name

    objects = None
    with open(sys.argv[i], "r") as f:
        objects = json.loads(f.read())
    name = os.path.basename(sys.argv[i]).split(".")[0]
    files.append(name)

    # Get all enums, bitfields, and structs
    for s in objects:
        if s["type"] == "enum":
            for o in range(len(s["options"])):
                s["options"][o] = make_name_fun(s["options"][o], True)
            all_enums.append(s)
        elif s["type"] == "bitfield":
            for f in range(len(s["fields"])):
                s["fields"][f] = make_name_fun(s["fields"][f], False)
            all_bitfields.append(s)
        elif s["type"] == "struct":
            for f in s["fields"]:
                if ("cache_only" in f and f["cache_only"]) and ("default" in f):
                    print("{}::{} - default AND cache_only cannot be used together in a field since they may be unexpectedly modified".format(s["name"],f["name"]), file=sys.stderr)
                    sys.exit(1)
                if f["type"] != "pad":
                    f["name"] = make_name_fun(f["name"], False)
                if f["type"] == "TagDependency":
                    # Superclasses
                    def expand_superclass(arr, superclass, subclass):
                        for i in arr:
                            if i == superclass:
                                for s in subclass:
                                    arr.append(s)
                                break
                    expand_superclass(f["classes"], "object", ["unit", "device", "item", "projectile", "scenery"])
                    expand_superclass(f["classes"], "unit", ["vehicle", "biped"])
                    expand_superclass(f["classes"], "item", ["weapon", "garbage", "equipment"])
                    expand_superclass(f["classes"], "shader", ["shader_environment", "shader_model", "shader_transparent_chicago", "shader_transparent_chicago_extended", "shader_transparent_glass", "shader_transparent_meter", "shader_transparent_plasma", "shader_transparent_water"])
                    expand_superclass(f["classes"], "device", ["device_control", "device_light_fixture", "device_machine"])
            all_structs.append(s)
        else:
            print("Unknown object type {}".format(s["type"]), file=sys.stderr)
            sys.exit(1)

# Basically, we're going to be rearranging structs so that structs that don't have dependencies get added first. Structs that do get added last, and they only get added once their dependencies are added
all_structs_arranged = []
def add_struct(name):
    # Make sure we aren't already added
    for s in all_structs_arranged:
        if s["name"] == name:
            return

    # Get all their dependencies
    dependencies = []

    # Get the struct
    struct_to_add = None
    for s in all_structs:
        if s["name"] == name:
            struct_to_add = s
            break

    if not struct_to_add:
        if name != "PredictedResource":
            print("Warning: Unknown struct {}".format(name), file=sys.stderr)
        return

    if "inherits" in struct_to_add:
        dependencies.append(struct_to_add["inherits"])

    for f in s["fields"]:
        if f["type"] == "TagReflexive":
            if f["struct"] not in dependencies:
                dependencies.append(f["struct"])

    for d in dependencies:
        add_struct(d)

    all_structs_arranged.append(struct_to_add)

for s in all_structs:
    add_struct(s["name"])

def to_hex(number):
    return "0x{:X}".format(number)

with open(sys.argv[1], "w") as f:
    with open(sys.argv[9], "w") as ecpp:
        make_definitions(f, ecpp, all_enums, all_bitfields, all_structs_arranged)

with open(sys.argv[2], "w") as hpp:
    with open(sys.argv[3], "w") as cpp_save_hek_data:
        with open(sys.argv[4], "w") as cpp_read_cache_file_data:
            with open(sys.argv[5], "w") as cpp_read_hek_data:
                with open(sys.argv[6], "w") as cpp_cache_format_data:
                    with open(sys.argv[7], "w") as cpp_cache_deformat_data:
                        with open(sys.argv[8], "w") as cpp_refactor_reference:
                            make_parser(all_enums, all_bitfields, all_structs_arranged, all_structs, extract_hidden, hpp, cpp_save_hek_data, cpp_read_cache_file_data, cpp_read_hek_data, cpp_cache_format_data, cpp_cache_deformat_data, cpp_refactor_reference)
