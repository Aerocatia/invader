# SPDX-License-Identifier: GPL-3.0-only

def make_parse_hek_tag_file(definition, append_line):
    for d in definition["definitions"]:
        if d["type"] == "struct":
            append_line("{} {}::parse_hek_tag_file(const std::byte *data, std::size_t data_size, bool postprocess) {{".format(d["name"], d["name"]))
            append_line("TagFileHeader::validate_header(reinterpret_cast<const TagFileHeader *>(data), data_size);", 1)
            append_line("std::size_t data_read = 0;", 1)
            append_line("std::size_t expected_data_read = data_size - sizeof(TagFileHeader);", 1)
            append_line("auto r = parse_hek_tag_data(data + sizeof(TagFileHeader), expected_data_read, data_read, postprocess);", 1)
            append_line("if(data_read != expected_data_read) {", 1)
            append_line("eprintf_error(\"invalid tag file; tag data was left over\");", 1)
            append_line("throw InvalidTagDataException();", 2)
            append_line("}", 1)
            append_line("return r;", 1)
            append_line("}")
