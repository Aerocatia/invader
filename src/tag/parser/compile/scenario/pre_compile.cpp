// SPDX-License-Identifier: GPL-3.0-only

#include <map>
#include <cassert>
#include <invader/tag/parser/parser.hpp>
#include <invader/build/build_workload.hpp>
#include <invader/file/file.hpp>
#include <invader/tag/parser/compile/scenario.hpp>

#include <riat/riat.hpp>

namespace Invader::Parser {
    static void merge_child_scenarios(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario);
    static void check_palettes(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario);
    static void fix_script_data(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, Scenario &scenario);
    static void fix_bsp_transitions(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario);
    
    void compile_scripts(Scenario &scenario, const HEK::GameEngineInfo &info, RIAT_OptimizationLevel optimization_level, std::vector<std::string> &warnings, const std::optional<std::vector<std::pair<std::string, std::vector<std::byte>>>> &script_source) {
        // Instantiate it
        RIAT::Instance instance;
        instance.set_compile_target(info.scenario_script_compile_target);
        instance.set_optimization_level(optimization_level);
        instance.set_user_data(&warnings);
    
        // Any warnings get eaten up here
        instance.set_warn_callback([](RIAT_Instance *instance, const char *message, const char *file, std::size_t line, std::size_t column) {
            char fmt_message[512];
            std::snprintf(fmt_message, sizeof(fmt_message), "%s:%zu:%zu: warning: %s", file, line, column, message);
            reinterpret_cast<std::vector<std::string> *>(riat_instance_get_user_data(instance))->emplace_back(fmt_message);
        });
        
        // Load the input from script_source
        decltype(scenario.source_files) source_files;
        if(script_source.has_value()) {
            for(auto &source : *script_source) {
                auto &file = source_files.emplace_back();
                
                // Check if it's too long. If not, copy. Otherwise, error
                if(source.first.size() > sizeof(file.name.string) - 1) {
                    eprintf_error("Script file name '%s' is too long", source.first.c_str());
                    throw std::exception();
                }
                std::strncpy(file.name.string, source.first.c_str(), sizeof(file.name.string) - 1);
                file.source = source.second;
            };
        }
        
        // Use the scenario tag's source data
        else {
            source_files = scenario.source_files;
        }
        
        // Load the scripts
        try {
            for(auto &source : source_files) {
                instance.load_script_source(reinterpret_cast<const char *>(source.source.data()), source.source.size(), (std::string(source.name.string) + ".hsc").c_str());
            }
            instance.compile_scripts();
        }
        catch(std::exception &e) {
            eprintf_error("Script compilation error: %s", e.what());
            throw InvalidTagDataException();
        }
        
        std::size_t node_limit = info.maximum_scenario_script_nodes;
        
        auto scripts = instance.get_scripts();
        auto globals = instance.get_globals();
        auto nodes = instance.get_nodes();
        
        std::size_t node_count = nodes.size();
        
        if(nodes.size() > node_limit) {
            eprintf_error("Node limit exceeded for the target engine (%zu > %zu)", node_count, node_limit);
            throw InvalidTagDataException();
        }
        
        std::vector<Invader::Parser::ScenarioScriptNode> into_nodes;
        
        auto format_index_to_id = [](std::size_t index) -> std::uint32_t {
            auto index_16_bit = static_cast<std::uint16_t>(index);
            return static_cast<std::uint32_t>(((index_16_bit + 0x6373) | 0x8000) << 16) | index_16_bit;
        };
        
        std::map<std::string, std::size_t> string_index;
        std::vector<std::byte> string_data;
        
        for(std::size_t node_index = 0; node_index < node_count; node_index++) {
            auto &n = nodes[node_index];
            auto &new_node = into_nodes.emplace_back();
            new_node = {};
            
            // Set the salt
            new_node.salt = format_index_to_id(node_index) >> 16;
            
            // If we have string data, add it
            if(n.string_data != NULL) {
                std::string str = n.string_data;
                if(!string_index.contains(str)) {
                    string_index[str] = string_data.size();
                    const auto *cstr = str.c_str();
                    string_data.insert(string_data.end(), reinterpret_cast<const std::byte *>(cstr), reinterpret_cast<const std::byte *>(cstr) + str.size());
                    string_data.emplace_back(std::byte());
                }
                new_node.string_offset = string_index[str];
            }
            
            // All nodes are marked with this...?
            new_node.flags |= Invader::HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_GARBAGE_COLLECTABLE;
            
            // Here's the type
            new_node.type = static_cast<Invader::HEK::ScenarioScriptValueType>(n.type);
            new_node.index_union = new_node.type;
            
            // Set this stuff
            if(n.is_primitive) {
                new_node.flags |= Invader::HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_PRIMITIVE;
                if(n.is_global) {
                    new_node.flags |= Invader::HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_GLOBAL;
                }
                else {
                    switch(n.type) {
                        case RIAT_ValueType::RIAT_VALUE_TYPE_BOOLEAN:
                            new_node.data.bool_int = n.bool_int;
                            break;
                        case RIAT_ValueType::RIAT_VALUE_TYPE_SCRIPT:
                        case RIAT_ValueType::RIAT_VALUE_TYPE_SHORT:
                            new_node.data.short_int = n.short_int;
                            break;
                        case RIAT_ValueType::RIAT_VALUE_TYPE_LONG:
                            new_node.data.long_int = n.long_int;
                            break;
                        case RIAT_ValueType::RIAT_VALUE_TYPE_REAL:
                            new_node.data.real = n.real;
                            break;
                        default:
                            break;
                    }
                }
            }
            else {
                new_node.data.tag_id.id = format_index_to_id(n.child_node);
                
                if(n.is_script_call) {
                    new_node.flags |= Invader::HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_SCRIPT_CALL;
                    new_node.index_union = n.call_index;
                }
            }
            
            // Set the next node?
            if(n.next_node == SIZE_MAX) {
                new_node.next_node = UINT32_MAX;
            }
            else {
                new_node.next_node = format_index_to_id(n.next_node);
            }
            
            // Get the index of the thing
            auto find_thing = [&n, &warnings, &source_files, &new_node](auto &array) -> void {
                auto len = array.size();
                bool exists = false;
                bool multiple_instances = false;
                size_t first_instance = 0;
                
                // See if it exists and then find the first multiple instance if it does
                for(std::size_t i = 0; i < len && !multiple_instances; i++) {
                    const char *c = n.string_data;
                    const char *d = array[i].name.string;
                    
                    while(*c != 0 && *d != 0 && std::tolower(*c) == std::tolower(*d)) {
                        c++;
                        d++;
                    }
                    
                    if(std::tolower(*c) == std::tolower(*d)) {
                        if(exists) {
                            multiple_instances = true;
                            break;
                        }
                        
                        first_instance = i;
                        exists = true;
                    }
                }
                
                if(!exists) {
                    throw std::exception();
                }
                
                if(multiple_instances) {
                    char warning[512];
                    std::snprintf(warning, sizeof(warning), "%s:%zu:%zu: warning: multiple instances of %s '%s' found (first instance is %zu)", source_files[n.file].name.string, n.line, n.column, HEK::ScenarioScriptValueType_to_string_pretty(new_node.type), n.string_data, first_instance);
                    warnings.emplace_back(warning);
                }
            };
            
            // Make sure the thing it refers to exists
            try {
                if(n.is_primitive && !n.is_global) {
                    switch(new_node.type) {
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_CUTSCENE_CAMERA_POINT:
                            find_thing(scenario.cutscene_camera_points);
                            break;
                            
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_CUTSCENE_FLAG:
                            find_thing(scenario.cutscene_flags);
                            break;
                            
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_CUTSCENE_RECORDING:
                            find_thing(scenario.recorded_animations);
                            break;
                            
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_CUTSCENE_TITLE:
                            find_thing(scenario.cutscene_titles);
                            break;
                            
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_DEVICE_GROUP:
                            find_thing(scenario.device_groups);
                            break;
                            
                        case HEK::ScenarioScriptValueType::SCENARIO_SCRIPT_VALUE_TYPE_OBJECT_NAME:
                            find_thing(scenario.object_names);
                            break;
                        
                        default:
                            break;
                    }
                }
            }
            catch(std::exception &) {
                eprintf_error("%s:%zu:%zu: error: can't find %s '%s'", source_files[n.file].name.string, n.line, n.column, HEK::ScenarioScriptValueType_to_string_pretty(new_node.type), n.string_data);
                throw InvalidTagDataException();
            }
        }
        
        using node_table_header_tag_fmt = Invader::Parser::ScenarioScriptNodeTable::struct_big;
        using node_tag_fmt = std::remove_reference<decltype(*into_nodes.data())>::type::struct_big;
        
        // Initialize the syntax data and write to it
        std::vector<std::byte> syntax_data(sizeof(node_table_header_tag_fmt) + node_limit * sizeof(node_tag_fmt));
        auto &table_output = *reinterpret_cast<node_table_header_tag_fmt *>(syntax_data.data());
        auto *node_output = reinterpret_cast<node_tag_fmt *>(&table_output + 1);
        table_output.count = node_count;
        table_output.size = node_count;
        table_output.maximum_count = node_limit;
        table_output.next_id = format_index_to_id(node_count) >> 16;
        table_output.element_size = sizeof(node_tag_fmt);
        table_output.data = 0x64407440;
        std::strncpy(table_output.name.string, "script node", sizeof(table_output.name.string));
        table_output.one = 1;
        for(std::size_t node_index = 0; node_index < node_count; node_index++) {
            auto output = into_nodes[node_index].generate_hek_tag_data();
            assert(sizeof(node_output[node_index]) == output.size());
            std::memcpy(&node_output[node_index], output.data(), output.size());
        }
        
        std::size_t script_count = scripts.size();
        std::size_t global_count = globals.size();
        
        // Set up scripts
        decltype(scenario.scripts) new_scripts;
        new_scripts.resize(script_count);
        for(std::size_t s = 0; s < script_count; s++) {
            auto &new_script = new_scripts[s];
            const auto &cmp_script = scripts[s];
            
            static_assert(sizeof(new_script.name.string) == sizeof(cmp_script.name));
            memcpy(new_script.name.string, cmp_script.name, sizeof(cmp_script.name));
            
            new_script.return_type = static_cast<decltype(new_script.return_type)>(cmp_script.return_type);
            new_script.script_type = static_cast<decltype(new_script.script_type)>(cmp_script.script_type);
            new_script.root_expression_index = format_index_to_id(cmp_script.first_node);
        }
        
        // Set up globals
        decltype(scenario.globals) new_globals;
        new_globals.resize(global_count);
        for(std::size_t g = 0; g < global_count; g++) {
            auto &new_global = new_globals[g];
            const auto &cmp_global = globals[g];
            
            static_assert(sizeof(new_global.name.string) == sizeof(cmp_global.name));
            memcpy(new_global.name.string, cmp_global.name, sizeof(cmp_global.name));
            
            new_global.type = static_cast<decltype(new_global.type)>(cmp_global.value_type);
            new_global.initialization_expression_index = format_index_to_id(cmp_global.first_node);
        }
        
        string_data.resize(string_data.size() + 1024);
        
        // Clear out the script data
        scenario.scripts = std::move(new_scripts);
        scenario.globals = std::move(new_globals);
        scenario.source_files = std::move(source_files);
        scenario.script_string_data = std::move(string_data);
        scenario.script_syntax_data = std::move(syntax_data);
    }
    
    void Scenario::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, std::size_t) {
        merge_child_scenarios(workload, tag_index, *this);

        if(!workload.cache_file_type.has_value()) {
            workload.cache_file_type = this->type;
            workload.demo_ui = this->flags & HEK::ScenarioFlagsFlag::SCENARIO_FLAGS_FLAG_USE_DEMO_UI;
        }

        // Check some things
        check_palettes(workload, tag_index, *this);
        fix_script_data(workload, tag_index, struct_index, *this);
        fix_bsp_transitions(workload, tag_index, *this);
    }
    
    static void fix_bsp_transitions(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario) {
        // BSP transitions
        std::size_t trigger_volume_count = scenario.trigger_volumes.size();
        scenario.bsp_switch_trigger_volumes.clear();
        for(std::size_t tv = 0; tv < trigger_volume_count; tv++) {
            auto &trigger_volume = scenario.trigger_volumes[tv];
            if(std::strncmp(trigger_volume.name.string, "bsp", 3) != 0) {
                continue;
            }

            // Parse it
            unsigned int bsp_from = ~0;
            unsigned int bsp_to = ~0;
            if(std::sscanf(trigger_volume.name.string, "bsp%u,%u", &bsp_from, &bsp_to) != 2) {
                continue;
            }

            // Save it
            if(bsp_from >= scenario.structure_bsps.size() || bsp_to >= scenario.structure_bsps.size()) {
                if(!workload.disable_error_checking) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Trigger volume #%zu (%s) references an invalid BSP index", tv, trigger_volume.name.string);
                    throw InvalidTagDataException();
                }
            }
            else {
                auto &bsp_switch_trigger_volume = scenario.bsp_switch_trigger_volumes.emplace_back();
                bsp_switch_trigger_volume.trigger_volume = static_cast<HEK::Index>(tv);
                bsp_switch_trigger_volume.source = static_cast<HEK::Index>(bsp_from);
                bsp_switch_trigger_volume.destination = static_cast<HEK::Index>(bsp_to);
                bsp_switch_trigger_volume.unknown = 0xFFFF;
            }
        }
    }
    
    static void fix_script_data(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, Scenario &scenario) {
        // If we have scripts, do stuff
        if((scenario.scripts.size() > 0 || scenario.globals.size() > 0) && scenario.source_files.size() == 0) {
            if(!workload.disable_error_checking) {
                workload.report_error(BuildWorkload::ErrorType::ERROR_TYPE_FATAL_ERROR, "Scenario tag has script data but no source file data", tag_index);
                eprintf_warn("To fix this, recompile the scripts");
                throw InvalidTagDataException();
            }
        }
        
        // Recompile scripts
        try {
            std::vector<std::string> warnings;
            compile_scripts(scenario, HEK::GameEngineInfo::get_game_engine_info(workload.get_build_parameters()->details.build_game_engine), workload.get_build_parameters()->script_optimization_level, warnings);
            for(auto &w : warnings) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING, tag_index, "Script compilation warning: %s", w.c_str());
            }
        }
        catch(std::exception &e) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Failed to compile scripts: %s", e.what());
            throw;
        }
        
        // Check for stubs and warn
        for(auto &script : scenario.scripts) {
            if(script.script_type == HEK::ScenarioScriptType::SCENARIO_SCRIPT_TYPE_STUB) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING, tag_index, "Script '%s' is a stub script but has not been replaced by a static script. It will function as a static script, instead.", script.name.string);
            }
        }
        
        // Is the syntax data correct?
        auto syntax_data_size = scenario.script_syntax_data.size();
        std::size_t expected_max_element_count = workload.get_build_parameters()->details.build_scenario_maximum_script_nodes;
        std::size_t correct_syntax_data_size = sizeof(ScenarioScriptNodeTable::struct_big) + expected_max_element_count * sizeof(ScenarioScriptNode::struct_big);
        if(scenario.script_syntax_data.size() != correct_syntax_data_size) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Script syntax data is incorrect for the target engine (%zu != %zu)", syntax_data_size, correct_syntax_data_size);
            throw InvalidTagDataException();
        }
        
        // Flip the endianness
        auto t = *reinterpret_cast<ScenarioScriptNodeTable::struct_big *>(scenario.script_syntax_data.data());
        *reinterpret_cast<ScenarioScriptNodeTable::struct_little *>(scenario.script_syntax_data.data()) = t;
        t.first_element_ptr = 0;
        
        auto *start_big = reinterpret_cast<ScenarioScriptNode::struct_big *>(scenario.script_syntax_data.data() + sizeof(t));
        auto *start_little = reinterpret_cast<ScenarioScriptNode::struct_little *>(start_big);
        
        // Make sure the element count is correct
        std::size_t max_element_count = t.maximum_count;
        if(max_element_count != expected_max_element_count) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Script syntax node count is wrong for the target engine (%zu != %zu)", max_element_count, expected_max_element_count);
            throw InvalidTagDataException();
        }
        
        // And now flip the endianness of the nodes
        for(std::size_t i = 0; i < max_element_count; i++) {
            start_little[i] = start_big[i];
        }

        // Get these things
        BuildWorkload::BuildWorkloadStruct script_data_struct = {};
        script_data_struct.data = std::move(scenario.script_syntax_data);
        const char *string_data = reinterpret_cast<const char *>(scenario.script_string_data.data());
        std::size_t string_data_length = scenario.script_string_data.size();
        
        // For verifying if strings end with 00 bytes down below
        while(string_data_length > 0) {
            if(string_data[string_data_length - 1] != 0) {
                string_data_length--;
            }
            else {
                break;
            }
        }

        const char *string_data_end = string_data + string_data_length;
        
        auto *syntax_data = script_data_struct.data.data();
        auto &table_header = *reinterpret_cast<ScenarioScriptNodeTable::struct_little *>(syntax_data);
        std::uint16_t element_count = table_header.size.read();
        auto *nodes = reinterpret_cast<ScenarioScriptNode::struct_little *>(&table_header + 1);

        for(std::uint16_t i = 0; i < element_count; i++) {
            // Check if we know the class
            std::optional<TagFourCC> tag_class;
            auto &node = nodes[i];

            // Check the class type
            switch(node.type.read()) {
                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_SOUND:
                    tag_class = HEK::TAG_FOURCC_SOUND;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_EFFECT:
                    tag_class = HEK::TAG_FOURCC_EFFECT;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_DAMAGE:
                    tag_class = HEK::TAG_FOURCC_DAMAGE_EFFECT;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_LOOPING_SOUND:
                    tag_class = HEK::TAG_FOURCC_SOUND_LOOPING;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_ANIMATION_GRAPH:
                    tag_class = HEK::TAG_FOURCC_MODEL_ANIMATIONS;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_ACTOR_VARIANT:
                    tag_class = HEK::TAG_FOURCC_ACTOR_VARIANT;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_DAMAGE_EFFECT:
                    tag_class = HEK::TAG_FOURCC_DAMAGE_EFFECT;
                    break;

                case HEK::SCENARIO_SCRIPT_VALUE_TYPE_OBJECT_DEFINITION:
                    tag_class = HEK::TAG_FOURCC_OBJECT;
                    break;

                default:
                    continue;
            }

            if(tag_class.has_value()) {
                // Check if we should leave it alone
                auto flags = node.flags.read();
                if(
                    (flags & HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_GLOBAL) ||
                    (flags & HEK::ScenarioScriptNodeFlagsFlag::SCENARIO_SCRIPT_NODE_FLAGS_FLAG_IS_SCRIPT_CALL)
                ) {
                    continue;
                }

                // Get the string
                const char *string = string_data + node.string_offset.read();
                if(string >= string_data_end) {
                    if(!workload.disable_error_checking) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Script node #%zu has an invalid string offset. The scripts need recompiled.", static_cast<std::size_t>(i));
                    }
                    break;
                }

                // Add it to the list
                std::size_t dependency_offset = reinterpret_cast<const std::byte *>(&node.data) - syntax_data;
                std::size_t new_id = workload.compile_tag_recursively(string, *tag_class);
                node.data = HEK::TagID { static_cast<std::uint32_t>(new_id) };
                auto &dependency = script_data_struct.dependencies.emplace_back();
                dependency.offset = dependency_offset;
                dependency.tag_id_only = true;
                dependency.tag_index = new_id;

                // Let's also add up a reference too. This is 110% pointless and only wastes tag data space, but it's what tool.exe does, and a Vap really wanted it.
                bool exists = false;
                auto &new_tag = workload.tags[new_id];
                for(auto &r : scenario.references) {
                    if(r.reference.tag_fourcc == new_tag.tag_fourcc && r.reference.path == new_tag.path) {
                        exists = true;
                        break;
                    }
                }
                if(!exists) {
                    auto &reference = scenario.references.emplace_back().reference;
                    reference.tag_fourcc = new_tag.tag_fourcc;
                    reference.path = new_tag.path;
                    reference.tag_id = HEK::TagID { static_cast<std::uint32_t>(new_id) };
                }
            }
        }

        // Add the new structs
        auto &new_ptr = workload.structs[struct_index].pointers.emplace_back();
        auto &scenario_struct = *reinterpret_cast<Scenario::struct_little *>(workload.structs[struct_index].data.data());
        scenario_struct.script_syntax_data.size = static_cast<std::uint32_t>(script_data_struct.data.size());
        new_ptr.offset = reinterpret_cast<std::byte *>(&scenario_struct.script_syntax_data.pointer) - reinterpret_cast<std::byte *>(&scenario_struct);
        new_ptr.struct_index = workload.structs.size();
        workload.structs.emplace_back(std::move(script_data_struct));
    }
    
    static void check_palettes(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario) {
        // Check for unused stuff
        std::size_t name_count = scenario.object_names.size();
        std::vector<std::vector<std::pair<const char *, std::size_t>>> name_used(name_count);

        // We want to make sure things are valid
        #define CHECK_PALETTE_AND_SPAWNS(object_type_str, scenario_object_type, scenario_palette_type, object_type_int) { \
            std::size_t type_count = scenario.scenario_palette_type.size(); \
            std::size_t count = scenario.scenario_object_type.size(); \
            std::vector<std::uint32_t> used(type_count); \
            for(std::size_t i = 0; i < count; i++) { \
                auto &r = scenario.scenario_object_type[i]; \
                std::size_t name_index = r.name; \
                if(name_index != NULL_INDEX) { \
                    /* Check the name to see if it's valid */ \
                    if(name_index >= name_count) { \
                        if(!workload.disable_error_checking) { \
                            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, object_type_str " spawn #%zu has an invalid name index (%zu >= %zu)", i, name_index, name_count); \
                            throw InvalidTagDataException(); \
                        } \
                    } \
                    /* If it is, increment the used counter and assign everything */ \
                    else { \
                        name_used[name_index].emplace_back(object_type_str, i); \
                        auto &name = scenario.object_names[name_index]; \
                        name.object_index = static_cast<HEK::Index>(i); \
                        name.object_type = HEK::ObjectType::object_type_int; \
                    } \
                } \
                std::size_t type_index = r.type; \
                if(type_index == NULL_INDEX) { \
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, object_type_str " spawn #%zu has no object type, so it will be unused", i); \
                } \
                else if(type_index >= type_count) { \
                    if(!workload.disable_error_checking) { \
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, object_type_str " spawn #%zu has an invalid type index (%zu >= %zu)", i, type_index, type_count); \
                        throw InvalidTagDataException(); \
                    } \
                } \
                else { \
                    used[type_index]++; \
                } \
            } \
            for(std::size_t i = 0; i < type_count; i++) { \
                auto &palette = scenario.scenario_palette_type[i].name; \
                bool is_null = palette.path.size() == 0; \
                if(!used[i]) { \
                    if(is_null) { \
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, object_type_str " palette type #%zu (null) is unused", i); \
                    } \
                    else { \
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, object_type_str " palette type #%zu (%s.%s) is unused", i, File::halo_path_to_preferred_path(palette.path).c_str(), HEK::tag_fourcc_to_extension(palette.tag_fourcc)); \
                    } \
                } \
                else if(is_null) { \
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, object_type_str " palette type #%zu is null, so %zu reference%s will be unused", i, static_cast<std::size_t>(used[i]), used[i] == 1 ? "" : "s"); \
                } \
            } \
        }

        CHECK_PALETTE_AND_SPAWNS("Biped", bipeds, biped_palette, OBJECT_TYPE_BIPED);
        CHECK_PALETTE_AND_SPAWNS("Vehicle", vehicles, vehicle_palette, OBJECT_TYPE_VEHICLE);
        CHECK_PALETTE_AND_SPAWNS("Weapon", weapons, weapon_palette, OBJECT_TYPE_WEAPON);
        CHECK_PALETTE_AND_SPAWNS("Equipment", equipment, equipment_palette, OBJECT_TYPE_EQUIPMENT);
        CHECK_PALETTE_AND_SPAWNS("Scenery", scenery, scenery_palette, OBJECT_TYPE_SCENERY);
        CHECK_PALETTE_AND_SPAWNS("Machine", machines, machine_palette, OBJECT_TYPE_DEVICE_MACHINE);
        CHECK_PALETTE_AND_SPAWNS("Control", controls, control_palette, OBJECT_TYPE_DEVICE_CONTROL);
        CHECK_PALETTE_AND_SPAWNS("Light fixture", light_fixtures, light_fixture_palette, OBJECT_TYPE_DEVICE_LIGHT_FIXTURE);
        CHECK_PALETTE_AND_SPAWNS("Sound scenery", sound_scenery, sound_scenery_palette, OBJECT_TYPE_SOUND_SCENERY);

        #undef CHECK_PALETTE_AND_SPAWNS
        
        // Next, let's make sure "set new name" is used
        for(auto &c : scenario.ai_conversations) {
            for(auto &p : c.participants) {
                auto new_name = p.set_new_name;
                if(new_name > name_count || new_name == NULL_INDEX) {
                    continue;
                }
                else if(name_used[new_name].size() == 0) {
                    name_used[new_name].emplace_back(); 
                }
            }
        }

        // Make sure we don't have any fun stuff with object names going on
        for(std::size_t i = 0; i < name_count; i++) {
            auto &used_arr = name_used[i];
            auto used = used_arr.size();
            auto &name = scenario.object_names[i];
            const char *name_str = name.name.string;
            if(used == 0) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING, tag_index, "Object name #%zu (%s) is unused", i, name_str);
            }
            else if(used > 1 && !workload.disable_error_checking) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Object name #%zu (%s) is used multiple times (found %zu times)", i, name_str, used);
                
                // Put together a list to help the user track everything down
                char found[1024] = {};
                std::size_t p = 0;
                
                std::size_t f = 0;
                for(auto &u : used_arr) {
                    // Don't show more than 3 elements
                    if(f++ == 3) {
                        std::snprintf(found + p, sizeof(found) - p, ", ...");
                        break;
                    }
                    else {
                        p += std::snprintf(found + p, sizeof(found) - p, "%s%s #%zu", f == 1 ? "" : ", ", u.first, u.second);
                        if(p > sizeof(found)) {
                            break;
                        }
                    }
                }
                
                // List everything off
                eprintf_warn_lesser("    - objects with this name: [%s]", found);
                throw InvalidTagDataException();
            }
        }
    }
    
    static void merge_child_scenario(Scenario &base_scenario, const Scenario &scenario_to_merge, BuildWorkload &workload, std::size_t tag_index, const char *child_scenario_path) {
        #define MERGE_ARRAY(what, condition) for(auto &merge : scenario_to_merge.what) { \
            bool can_merge = true; \
            for([[maybe_unused]] auto &base : base_scenario.what) { \
                if(!(condition)) { \
                    can_merge = false; \
                    break; \
                } \
            } \
            if(can_merge) { \
                base_scenario.what.emplace_back(merge); \
            } \
        }
        
        MERGE_ARRAY(child_scenarios, true);
        MERGE_ARRAY(functions, true);
        MERGE_ARRAY(comments, true);
        MERGE_ARRAY(object_names, merge.name != base.name);
        MERGE_ARRAY(device_groups, merge.name != base.name);
        MERGE_ARRAY(player_starting_profile, true);
        MERGE_ARRAY(trigger_volumes, merge.name != base.name);
        MERGE_ARRAY(recorded_animations, merge.name != base.name);
        MERGE_ARRAY(netgame_flags, true);
        MERGE_ARRAY(netgame_equipment, true);
        MERGE_ARRAY(starting_equipment, true);
        MERGE_ARRAY(actor_palette, merge.reference.path != base.reference.path || merge.reference.tag_fourcc != base.reference.tag_fourcc);
        MERGE_ARRAY(ai_animation_references, merge.animation_name != base.animation_name);
        MERGE_ARRAY(ai_script_references, merge.script_name != base.script_name);
        MERGE_ARRAY(ai_recording_references, merge.recording_name != base.recording_name);
        MERGE_ARRAY(references, merge.reference.path != base.reference.path || merge.reference.tag_fourcc != base.reference.tag_fourcc);
        MERGE_ARRAY(cutscene_flags, merge.name != base.name);
        MERGE_ARRAY(cutscene_camera_points, merge.name != base.name);
        MERGE_ARRAY(cutscene_titles, merge.name != base.name);
        MERGE_ARRAY(source_files, merge.name != base.name);
        MERGE_ARRAY(decal_palette, merge.reference.path != base.reference.path || merge.reference.tag_fourcc != base.reference.tag_fourcc);
        
        // Merge palettes
        #define MERGE_PALETTE(what) MERGE_ARRAY(what, merge.name.path != base.name.path || merge.name.tag_fourcc != base.name.tag_fourcc)
        
        MERGE_PALETTE(scenery_palette);
        MERGE_PALETTE(biped_palette);
        MERGE_PALETTE(vehicle_palette);
        MERGE_PALETTE(equipment_palette);
        MERGE_PALETTE(weapon_palette);
        MERGE_PALETTE(machine_palette);
        MERGE_PALETTE(control_palette);
        MERGE_PALETTE(light_fixture_palette);
        MERGE_PALETTE(sound_scenery_palette);
        
        // Make some lambdas for finding stuff quickly
        #define TRANSLATE_PALETTE(what, match_comparison) [&base_scenario, &scenario_to_merge, &workload, &tag_index, &child_scenario_path](HEK::Index old_index) -> HEK::Index { \
            /* If we're null, return null */ \
            if(old_index == NULL_INDEX) { \
                return NULL_INDEX; \
            } \
\
            /* if we're out of bounds, fail */ \
            auto old_count = scenario_to_merge.what.size(); \
            if(old_index >= old_count) { \
                if(!workload.disable_error_checking) { \
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, # what " index in child scenario %s is out of bounds (%zu >= %zu)", child_scenario_path, static_cast<std::size_t>(old_index), old_count); \
                    throw OutOfBoundsException(); \
                } \
                return NULL_INDEX; \
            } \
\
            /* Find it */ \
            auto &merge = scenario_to_merge.what[old_index]; \
            auto new_count = base_scenario.what.size(); \
            for(std::size_t name = 0; name < new_count; name++) { \
                auto &base = base_scenario.what[name]; \
                if((match_comparison)) { \
                    if(name >= NULL_INDEX) { \
                        if(!workload.disable_error_checking) { \
                            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, # what " in child scenario %s exceeded %zu when merging", child_scenario_path, static_cast<std::size_t>(NULL_INDEX - 1)); \
                            throw InvalidTagDataException(); \
                        } \
                        return NULL_INDEX; \
                    } \
                    return name; \
                } \
            } \
            if(!workload.disable_error_checking) { \
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Failed to find an entry in " # what " for child scenario %s", child_scenario_path); \
                throw OutOfBoundsException(); \
            } \
            return NULL_INDEX; \
        }
        auto translate_object_name = TRANSLATE_PALETTE(object_names, (merge.name == base.name));
        auto translate_device_group = TRANSLATE_PALETTE(device_groups, (merge.name == base.name));
        
        // Merge AI conversations
        for(auto &aic : scenario_to_merge.ai_conversations) {
            auto &new_aic = base_scenario.ai_conversations.emplace_back(aic);
            for(auto &p : new_aic.participants) {
                p.set_new_name = translate_object_name(p.set_new_name);
                p.use_this_object = translate_object_name(p.use_this_object);
            }
        }

        #undef MERGE_PALETTE
        #undef MERGE_ARRAY
        
        #define MERGE_OBJECTS_ALL(what, what_palette, ...) { \
            auto object_count = scenario_to_merge.what.size(); \
            auto translate_palette = TRANSLATE_PALETTE(what_palette, (merge.name.path == base.name.path && merge.name.tag_fourcc == base.name.tag_fourcc)); \
            for(std::size_t o = 0; o < object_count; o++) { \
                auto &new_element = base_scenario.what.emplace_back(scenario_to_merge.what[o]); \
                new_element.name = translate_object_name(new_element.name); \
                new_element.type = translate_palette(new_element.type); \
                __VA_ARGS__ \
            } \
        }
        
        #define MERGE_OBJECTS(what, what_palette) MERGE_OBJECTS_ALL(what, what_palette, {})
        #define MERGE_DEVICES(what, what_palette) MERGE_OBJECTS_ALL(what, what_palette, { \
            new_element.power_group = translate_device_group(new_element.power_group); \
            new_element.position_group = translate_device_group(new_element.position_group); \
        })
        
        MERGE_OBJECTS(scenery,scenery_palette);
        MERGE_OBJECTS(bipeds,biped_palette);
        MERGE_OBJECTS(vehicles,vehicle_palette);
        MERGE_OBJECTS(equipment,equipment_palette);
        MERGE_OBJECTS(weapons,weapon_palette);
        MERGE_DEVICES(machines,machine_palette);
        MERGE_DEVICES(controls,control_palette);
        MERGE_DEVICES(light_fixtures,light_fixture_palette);
        MERGE_OBJECTS(sound_scenery,sound_scenery_palette);
        
        #undef MERGE_OBJECTS
        #undef MERGE_OBJECTS_ALL
        
        // Decals
        auto translate_decal_palette = TRANSLATE_PALETTE(decal_palette, merge.reference.tag_fourcc == base.reference.tag_fourcc && merge.reference.path == base.reference.path);
        for(auto &decal : scenario_to_merge.decals) {
            // Add our new decal
            auto &new_decal = base_scenario.decals.emplace_back(decal);
            new_decal.decal_type = translate_decal_palette(new_decal.decal_type);
        }
        
        // AI stuff
        auto translate_actor_palette = TRANSLATE_PALETTE(actor_palette, (merge.reference.tag_fourcc == base.reference.tag_fourcc && merge.reference.path == base.reference.path));
        auto translate_animation_palette = TRANSLATE_PALETTE(ai_animation_references, merge.animation_name == base.animation_name);
        auto translate_command_list = TRANSLATE_PALETTE(command_lists, merge.name == base.name);
        auto translate_recording = TRANSLATE_PALETTE(ai_recording_references, merge.recording_name == base.recording_name);
        auto translate_script_reference = TRANSLATE_PALETTE(ai_script_references, merge.script_name == base.script_name);
        
        // Merge command lists
        for(auto &command_list : scenario_to_merge.command_lists) {
            // First, make sure we don't have this in here already
            bool exists = false;
            for(auto &existing_command_list : base_scenario.command_lists) {
                if(existing_command_list.name == command_list.name) {
                    exists = true;
                    break;
                }
            }
            // Darn
            if(exists) {
                continue;
            }
            
            // Add our new list
            auto &new_command_list = base_scenario.command_lists.emplace_back(command_list);
            for(auto &command : new_command_list.commands) {
                command.animation = translate_animation_palette(command.animation);
                command.recording = translate_recording(command.recording);
                command.object_name = translate_object_name(command.object_name);
                command.script = translate_script_reference(command.script);
            }
        }
        
        // Merge encounters
        for(auto &encounter : scenario_to_merge.encounters) {
            // First, make sure we don't have this in here already
            bool exists = false;
            for(auto &existing_encounters : base_scenario.encounters) {
                if(existing_encounters.name == encounter.name) {
                    exists = true;
                    break;
                }
            }
            // Darn
            if(exists) {
                continue;
            }
            
            // Add our new encounter
            auto &new_encounter = base_scenario.encounters.emplace_back(encounter);
            for(auto &squad : new_encounter.squads) {
                squad.actor_type = translate_actor_palette(squad.actor_type);
                for(auto &mp : squad.move_positions) {
                    mp.animation = translate_animation_palette(mp.animation);
                }
                for(auto &sl : squad.starting_locations) {
                    sl.actor_type = translate_actor_palette(sl.actor_type);
                    sl.command_list = translate_command_list(sl.command_list);
                }
            }
        }
        
        #undef TRANSLATE_PALETTE
    }

    static void merge_child_scenarios(BuildWorkload &workload, std::size_t tag_index, Scenario &scenario) {
        // Merge child scenarios
        if(!scenario.child_scenarios.empty() && !workload.disable_recursion) {
            // Let's begin by adding this scenario to the list (in case we reference ourself)
            std::vector<std::string> merged_scenarios;
            merged_scenarios.emplace_back(workload.tags[tag_index].path);
            
            // Take the scenario off the top
            while(scenario.child_scenarios.size()) {
                // Get the scenario
                auto first_scenario = scenario.child_scenarios[0].child_scenario;
                
                if(!first_scenario.path.empty()) {
                    // If this isn't even a scenario tag... what
                    if(first_scenario.tag_fourcc != TagFourCC::TAG_FOURCC_SCENARIO) {
                        // This should fail even if we aren't checking for errors because this is invalid
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Non-scenario %s.%s referenced in child scenarios", File::halo_path_to_preferred_path(first_scenario.path).c_str(), HEK::tag_fourcc_to_extension(first_scenario.tag_fourcc));
                        throw InvalidTagDataException();
                    }
                    
                    // Make sure we haven't done it already
                    for(auto &m : merged_scenarios) {
                        // This should fail even if we aren't checking for errors because this is invalid
                        if(m == first_scenario.path) {
                            workload.report_error(BuildWorkload::ErrorType::ERROR_TYPE_FATAL_ERROR, "Duplicate or cyclical child scenario references are present", tag_index);
                            eprintf_warn("First duplicate scenario: %s.%s", File::halo_path_to_preferred_path(first_scenario.path).c_str(), HEK::tag_fourcc_to_extension(first_scenario.tag_fourcc));
                            throw InvalidTagDataException();
                        }
                    }
                    
                    // Add it to the list
                    merged_scenarios.emplace_back(first_scenario.path);
                    
                    // Find it
                    char file_path_cstr[1024];
                    std::snprintf(file_path_cstr, sizeof(file_path_cstr), "%s.%s", File::halo_path_to_preferred_path(first_scenario.path).c_str(), HEK::tag_fourcc_to_extension(first_scenario.tag_fourcc));
                    auto file_path = File::tag_path_to_file_path(file_path_cstr, workload.get_build_parameters()->tags_directories);
                    if(!file_path.has_value() || !std::filesystem::exists(*file_path)) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Child scenario %s not found", file_path_cstr);
                        throw InvalidTagDataException();
                    }
                    
                    // Open it
                    auto data = File::open_file(*file_path);
                    if(!data.has_value()) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Failed to open %s", file_path->string().c_str());
                        throw InvalidTagDataException();
                    }
                    
                    // Parse and merge it
                    try {
                        auto child = Scenario::parse_hek_tag_file(data->data(), data->size());
                        data.reset(); // clear it
                        merge_child_scenario(scenario, child, workload, tag_index, (File::halo_path_to_preferred_path(first_scenario.path) + "." + HEK::tag_fourcc_to_extension(first_scenario.tag_fourcc)).c_str());
                    }
                    catch(std::exception &) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Failed to merge %s.%s into %s.%s",
                                            File::halo_path_to_preferred_path(first_scenario.path).c_str(),
                                            HEK::tag_fourcc_to_extension(first_scenario.tag_fourcc),
                                            File::halo_path_to_preferred_path(workload.tags[tag_index].path).c_str(),
                                            HEK::tag_fourcc_to_extension(workload.tags[tag_index].tag_fourcc)
                                           );
                        throw;
                    }
                }
                
                // Delete the scenario
                scenario.child_scenarios.erase(scenario.child_scenarios.begin());
            }
        }
    }

    void ScenarioCutsceneTitle::pre_compile(BuildWorkload &, std::size_t, std::size_t, std::size_t) {
        this->fade_in_time *= TICK_RATE;
        this->fade_out_time *= TICK_RATE;
        this->up_time *= TICK_RATE;
    }
    
    void ScenarioFiringPosition::pre_compile(BuildWorkload &, std::size_t, std::size_t, std::size_t) {
        this->cluster_index = NULL_INDEX;
        this->surface_index = NULL_INDEX;
    }
}
