// SPDX-License-Identifier: GPL-3.0-only

#include <invader/tag/parser/parser.hpp>
#include <invader/tag/hek/class/bitmap.hpp>
#include <invader/tag/parser/compile/scenario_structure_bsp.hpp>
#include <invader/build/build_workload.hpp>

namespace Invader::Parser {
    void ScenarioStructureBSP::pre_compile(BuildWorkload &, std::size_t, std::size_t, std::size_t) {
        this->runtime_decals.clear(); // delete these in case this tag was extracted improperly
    }
    
    bool ScenarioStructureBSPMaterial::check_for_nonnormal_vectors_more(bool normalize) {
        auto *vertices = this->uncompressed_vertices.data();
        auto uncompressed_vertices_size = this->uncompressed_vertices.size();

        auto *lightmap_rendered_vertices = reinterpret_cast<ScenarioStructureBSPMaterialUncompressedRenderedVertex::struct_little *>(vertices);
        auto *lightmap_lightmap_vertices = reinterpret_cast<ScenarioStructureBSPMaterialUncompressedLightmapVertex::struct_little *>(lightmap_rendered_vertices + this->rendered_vertices_count);
        
        auto *lightmap_vertices_end = lightmap_lightmap_vertices + this->lightmap_vertices_count;
        std::size_t expected_size = reinterpret_cast<std::byte *>(lightmap_vertices_end) - vertices;
        if(expected_size != uncompressed_vertices_size) {
            return false; // stuff's messed up. we can't do anything
        }
        
        bool return_value = false;
        
        if(this->rendered_vertices_count) {
            for(std::size_t i = 0; i < this->rendered_vertices_count; i++) {
                auto &rv = lightmap_rendered_vertices[i];
                if(!rv.normal.is_normalized()) {
                    if(!normalize) {
                        return true;
                    }
                    else {
                        return_value = true;
                        rv.normal = rv.normal.normalize();
                    }
                }
            }
        }
        
        return return_value;
    }

    void ScenarioStructureBSPCollisionMaterial::post_compile(BuildWorkload &workload, std::size_t, std::size_t struct_index, std::size_t offset) {
        auto *data = workload.structs[struct_index].data.data();
        auto &material = *reinterpret_cast<struct_little *>(data + offset);

        if(workload.disable_recursion) {
            material.material = static_cast<HEK::MaterialType>(0xFFFF);
            return;
        }

        this->material = reinterpret_cast<Shader::struct_little *>(workload.structs[(*workload.tags[this->shader.tag_id.index].base_struct)].data.data())->material_type;
        material.material = this->material;
    }
    
    template <typename RenderedVertex, typename LightmapVertex> static void check_bsp_vertices(const std::byte *vertices, std::size_t vertices_size, std::size_t rendered_vertex_count, std::size_t lightmap_vertex_count, BuildWorkload &workload, std::size_t tag_index) {
        const auto *lightmap_rendered_vertices = reinterpret_cast<const RenderedVertex *>(vertices);
        const auto *lightmap_lightmap_vertices = reinterpret_cast<const LightmapVertex *>(lightmap_rendered_vertices + rendered_vertex_count);
        auto *lightmap_vertices_end = lightmap_lightmap_vertices + lightmap_vertex_count;
        
        std::size_t expected_size = reinterpret_cast<const std::byte *>(lightmap_vertices_end) - vertices;
        if(expected_size != vertices_size) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "BSP lightmap material lightmap vertices size is wrong (%zu gotten, %zu expected)", expected_size, vertices_size);
            throw InvalidTagDataException();
        }
    }

    void ScenarioStructureBSP::post_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, std::size_t offset) {
        auto lightmap_count = this->lightmaps.size();
        auto lightmap_bitmap = this->lightmaps_bitmap.tag_id;
        
        // If we need to, handle lightmap bitmap stuff
        if(!workload.disable_recursion && !workload.disable_error_checking) {
            // How many lightmap bitmaps do we have
            std::size_t lightmap_bitmap_count;
            if(!lightmap_bitmap.is_null()) {
                lightmap_bitmap_count = reinterpret_cast<Bitmap::struct_little *>(workload.structs[*workload.tags[lightmap_bitmap.index].base_struct].data.data())->bitmap_data.count.read();
            }
            else {
                lightmap_bitmap_count = 0;
            }
            
            // Check lightmaps to see if they're valid
            std::size_t invalid_lightmap_bitmap_indices = 0;
            bool lightmaps_present = false;
            for(std::size_t i = 0; i < lightmap_count; i++) {
                auto &lm = this->lightmaps[i];
                
                // Do we even have lightmaps?
                for(auto &m : lm.materials) {
                    switch(m.shader.tag_fourcc) {
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_CHICAGO:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_CHICAGO_EXTENDED:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_GENERIC:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_GLASS:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_GLSL:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_METER:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_PLASMA:
                        case HEK::TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_WATER:
                            break;
                        default:
                            if(m.lightmap_vertices_count) {
                                lightmaps_present = true;
                            }
                            break;
                    }
                }
                
                // Make sure the bitmap is valid
                if(lm.bitmap != NULL_INDEX) {
                    lightmaps_present = true;
                    auto bitmap = static_cast<std::size_t>(lm.bitmap);
                    if(bitmap >= lightmap_bitmap_count) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_ERROR, tag_index, "BSP lightmap #%zu has an invalid bitmap index (%zu >= %zu)", i, bitmap, lightmap_bitmap_count);
                        invalid_lightmap_bitmap_indices++;
                    }
                }
            }
            
            // If we have invalid lightmap indices, error
            if(invalid_lightmap_bitmap_indices) {
                if(lightmap_bitmap.is_null()) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "No BSP lightmp bitmap is referenced, but %zu lightmap%s a non-null bitmap index", invalid_lightmap_bitmap_indices, invalid_lightmap_bitmap_indices == 1 ? " has" : "s have");
                }
                else {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "A BSP lightmp bitmap is referenced, but %zu lightmap%s an invalid bitmap index", invalid_lightmap_bitmap_indices, invalid_lightmap_bitmap_indices == 1 ? " has" : "s have");
                }
                eprintf_warn("Rebake your lightmaps to fix this error.");
                throw InvalidTagDataException();
            }

            if(!lightmaps_present) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING, tag_index, "BSP has no lightmaps baked, so parts of it will not render");
            }
        }

        // Handle the fog palette
        auto &tag_struct = workload.structs[struct_index];
        auto &tag_data = *reinterpret_cast<struct_little *>(tag_struct.data.data() + offset);

        std::size_t fog_plane_count = this->fog_planes.size();
        std::size_t fog_region_count = this->fog_regions.size();
        std::size_t fog_palette_count = this->fog_palette.size();
        
        // Handle fog plane materials
        if(fog_plane_count) {
            // Null these out by default (this is complete bullshit, but the game may crash if they aren't nulled without materials)
            auto fog_plane_index = *tag_struct.resolve_pointer(&tag_data.fog_planes.pointer);
            auto *fog_planes = reinterpret_cast<ScenarioStructureBSPFogPlane::struct_little *>(workload.structs[fog_plane_index].data.data());
            for(std::size_t i = 0; i < fog_plane_count; i++) {
                fog_planes[i].material_type = static_cast<HEK::MaterialType>(0xFFFF);
            }
            
            // If we *do* have fog palettes, regions, planes then we can determine their material
            if(fog_palette_count && fog_region_count) {
                auto fog_palette_index = *tag_struct.resolve_pointer(&tag_data.fog_palette.pointer);
                auto fog_region_index = *tag_struct.resolve_pointer(&tag_data.fog_regions.pointer);
                auto *fog_regions = reinterpret_cast<ScenarioStructureBSPFogRegion::struct_little *>(workload.structs[fog_region_index].data.data());
                auto *fog_palette = reinterpret_cast<ScenarioStructureBSPFogPalette::struct_little *>(workload.structs[fog_palette_index].data.data());

                // Go through each fog plane
                for(std::size_t i = 0; i < fog_plane_count; i++) {
                    if(workload.disable_recursion) {
                        continue;
                    }

                    // Find what region this fog is in
                    auto &plane = fog_planes[i];
                    std::size_t region_index = plane.front_region;
                    if(region_index >= fog_region_count) {
                        continue;
                    }
                    auto &region = fog_regions[region_index];

                    // Lastly get the fog tag
                    std::size_t palette_index = region.fog;
                    if(palette_index >= fog_palette_count) {
                        continue;
                    }

                    const auto &fog_id = fog_palette[palette_index].fog.tag_id.read();
                    if(fog_id.is_null()) {
                        continue;
                    }

                    auto &fog = *reinterpret_cast<Fog::struct_little *>(workload.structs[*workload.tags[fog_id.index].base_struct].data.data());
                    if(fog.flags & HEK::FogFlagsFlag::FOG_FLAGS_FLAG_IS_WATER) {
                        plane.material_type = HEK::MaterialType::MATERIAL_TYPE_WATER;
                    }
                }
            }
        }
    }
    
    void ScenarioStructureBSPDetailObjectData::pre_compile(BuildWorkload &, std::size_t, std::size_t, std::size_t) {
        this->bullshit = this->instances.size() != 0;
    }

    bool regenerate_missing_bsp_vertices(ScenarioStructureBSPMaterial &material, bool fix) {
        // Lightmap vertices size is incorrect
        if(material.lightmap_vertices_count != material.rendered_vertices_count && material.lightmap_vertices_count != 0) {
            eprintf_error("Can't fix: Non-zero lightmap vertex count is wrong (%zu != %zu)", static_cast<std::size_t>(material.lightmap_vertices_count), static_cast<std::size_t>(material.rendered_vertices_count));
            return false;
        }

        #define PROCESS_VERTICES(from,to,rendered_type_from,rendered_type_to,lightmap_type_from,lightmap_type_to,convert_rendered,convert_lightmap) \
            /* Extract vertices */ \
            std::size_t vertices_size = material.rendered_vertices_count * sizeof(rendered_type_from::struct_little); \
            std::size_t lightmap_vertices_size = material.lightmap_vertices_count * sizeof(lightmap_type_from::struct_little); \
            std::size_t total_vertices_size = lightmap_vertices_size + vertices_size; \
     \
            /* Make sure it isn't bullshit */ \
            if(total_vertices_size != material.from.size()) { \
                eprintf_error("Can't fix: Vertices is an invalid size (%zu != %zu)", total_vertices_size, material.from.size()); \
                return false; \
            } \
            \
            if(!fix) { \
                return true; \
            } \
     \
            const std::byte *bsp_vertices_start = material.from.data(); \
            const std::byte *lightmap_vertices_start = bsp_vertices_start + material.rendered_vertices_count * sizeof(rendered_type_from::struct_little); \
            const auto *bsp_vertices = reinterpret_cast<const rendered_type_from::struct_little *>(bsp_vertices_start); \
     \
            auto *new_bsp_vertices = reinterpret_cast<rendered_type_to::struct_little *>( \
                material.to.insert( \
                    material.to.end(), \
                    material.rendered_vertices_count * sizeof(rendered_type_to::struct_little), \
                    std::byte() \
                ).base() \
            ); \
            for(std::size_t v = 0; v < material.rendered_vertices_count; v++) { \
                new_bsp_vertices[v] = convert_rendered(bsp_vertices[v]); \
            } \
     \
            /* Add lightmap vertices */ \
            if(material.lightmap_vertices_count == material.rendered_vertices_count) { \
                const auto *bsp_lightmap_vertices = reinterpret_cast<const lightmap_type_from::struct_little *>(lightmap_vertices_start); \
     \
                /* Decompress them as well */ \
                auto *new_bsp_lightmap_vertices = reinterpret_cast<lightmap_type_to::struct_little *>( \
                    material.to.insert( \
                        material.to.end(), \
                        material.lightmap_vertices_count * sizeof(lightmap_type_to::struct_little), \
                        std::byte() \
                    ).base() \
                ); \
                for(std::size_t v = 0; v < material.lightmap_vertices_count; v++) { \
                    new_bsp_lightmap_vertices[v] = convert_lightmap(bsp_lightmap_vertices[v]); \
                } \
            }

        if(material.uncompressed_vertices.size() == 0 && material.compressed_vertices.size() != 0) {
            PROCESS_VERTICES(compressed_vertices,uncompressed_vertices,ScenarioStructureBSPMaterialCompressedRenderedVertex,ScenarioStructureBSPMaterialUncompressedRenderedVertex,ScenarioStructureBSPMaterialCompressedLightmapVertex,ScenarioStructureBSPMaterialUncompressedLightmapVertex,decompress_sbsp_rendered_vertex,decompress_sbsp_lightmap_vertex)
        }
        else if(material.uncompressed_vertices.size() != 0 && material.compressed_vertices.size() == 0) {
            PROCESS_VERTICES(uncompressed_vertices,compressed_vertices,ScenarioStructureBSPMaterialUncompressedRenderedVertex,ScenarioStructureBSPMaterialCompressedRenderedVertex,ScenarioStructureBSPMaterialUncompressedLightmapVertex,ScenarioStructureBSPMaterialCompressedLightmapVertex,compress_sbsp_rendered_vertex,compress_sbsp_lightmap_vertex)
        }
        else {
            return false;
        }
        return true;
    }

    bool regenerate_missing_bsp_vertices(ScenarioStructureBSP &bsp, bool fix) {
        bool return_value = false;
        for(auto &lightmap : bsp.lightmaps) {
            for(auto &material : lightmap.materials) {
                return_value = regenerate_missing_bsp_vertices(material, fix) || return_value;
            }
        }
        return return_value;
    }

    void ScenarioStructureBSPMaterial::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer> pointer) {
        // Do nothing if there is nothing to do
        if(this->rendered_vertices_count == 0) {
            this->lightmap_vertices_count = 0;
            return;
        }

        // Material
        auto &bsp_material = tag.get_struct_at_pointer<HEK::ScenarioStructureBSPMaterial>(*pointer);

        // If it's Xbox, it's compressed
        auto engine = tag.get_map().get_engine();
        if(engine == HEK::CacheFileEngine::CACHE_FILE_XBOX) {
            // Extract vertices
            std::size_t compressed_vertices_size = this->rendered_vertices_count * sizeof(ScenarioStructureBSPMaterialCompressedRenderedVertex::struct_little);
            std::size_t lightmap_vertices_size = this->lightmap_vertices_count * sizeof(ScenarioStructureBSPMaterialCompressedLightmapVertex::struct_little);
            std::size_t total_vertices_size = lightmap_vertices_size + compressed_vertices_size;
            const std::byte *compressed_bsp_vertices_start = tag.data(bsp_material.compressed_vertices.pointer, total_vertices_size);
            this->compressed_vertices = std::vector<std::byte>(compressed_bsp_vertices_start, compressed_bsp_vertices_start + total_vertices_size);
            if(!regenerate_missing_bsp_vertices(*this, true)) {
                eprintf_error("Failed to decompress vertices");
                throw InvalidTagDataException();
            }
        }
        else {
            // Extract vertices
            std::size_t uncompressed_vertices_size = this->rendered_vertices_count * sizeof(ScenarioStructureBSPMaterialUncompressedRenderedVertex::struct_little);
            std::size_t lightmap_vertices_size = this->lightmap_vertices_count * sizeof(ScenarioStructureBSPMaterialUncompressedLightmapVertex::struct_little);
            std::size_t total_vertices_size = lightmap_vertices_size + uncompressed_vertices_size;
            
            const std::byte *uncompressed_bsp_vertices_start = tag.data(bsp_material.uncompressed_vertices.pointer, total_vertices_size);
            const std::byte *uncompressed_lightmap_vertices_start = uncompressed_bsp_vertices_start + this->rendered_vertices_count * sizeof(ScenarioStructureBSPMaterialUncompressedRenderedVertex::struct_little);

            this->uncompressed_vertices = std::vector<std::byte>(uncompressed_bsp_vertices_start, uncompressed_bsp_vertices_start + uncompressed_vertices_size);
            this->uncompressed_vertices.insert(this->uncompressed_vertices.end(), uncompressed_lightmap_vertices_start, uncompressed_lightmap_vertices_start + lightmap_vertices_size);

            if(!regenerate_missing_bsp_vertices(*this, true)) {
                eprintf_error("Failed to compress vertices");
                throw InvalidTagDataException();
            }
        }
    }
    
    void ScenarioStructureBSPMaterial::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t, std::size_t) {
        if(workload.disable_error_checking) {
            return;
        }
        
        if(this->lightmap_vertices_count != 0 && this->lightmap_vertices_count != this->rendered_vertices_count) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "BSP lightmap material doesn't have equal # of lightmap and rendered vertices");
            throw InvalidTagDataException();
        }

        // Xbox uses compressed vertices
        if(workload.get_build_parameters()->details.build_cache_file_engine == HEK::CacheFileEngine::CACHE_FILE_XBOX) {
            check_bsp_vertices<Parser::ScenarioStructureBSPMaterialCompressedRenderedVertex::struct_little, Parser::ScenarioStructureBSPMaterialCompressedLightmapVertex::struct_little>(this->compressed_vertices.data(), this->compressed_vertices.size(), this->rendered_vertices_count, this->lightmap_vertices_count, workload, tag_index);
            this->uncompressed_vertices.clear();
            this->rendered_vertices_offset = this->rendered_vertices_count * sizeof(Parser::ScenarioStructureBSPMaterialCompressedRenderedVertex::struct_little);
            this->do_not_screw_up_the_model = 1;
            this->set_this_or_die = 3;
        }
        else {
            check_bsp_vertices<Parser::ScenarioStructureBSPMaterialUncompressedRenderedVertex::struct_little, Parser::ScenarioStructureBSPMaterialUncompressedLightmapVertex::struct_little>(this->uncompressed_vertices.data(), this->uncompressed_vertices.size(), this->rendered_vertices_count, this->lightmap_vertices_count, workload, tag_index);
            this->rendered_vertices_offset = this->rendered_vertices_count * sizeof(Parser::ScenarioStructureBSPMaterialUncompressedRenderedVertex::struct_little);
            this->compressed_vertices.clear();
        }
    }
    
    void set_up_xbox_cache_bsp_data(BuildWorkload &workload, std::size_t bsp_header_struct_index, std::size_t bsp_struct_index, std::size_t bsp) {
        // Add two structs
        auto struct_count = workload.structs.size();
        workload.structs.resize(struct_count + 2);
        auto rendered_vertices_struct_index = struct_count;
        auto lightmap_vertices_struct_index = struct_count + 1;
        
        // Let's get the header and base struct
        auto &bsp_header_struct = workload.structs[bsp_header_struct_index];
        auto &bsp_header = *reinterpret_cast<Parser::ScenarioStructureBSPCompiledHeader::struct_little *>(bsp_header_struct.data.data());
        auto &bsp_struct = workload.structs[bsp_struct_index];
        auto &bsp_data = *reinterpret_cast<Parser::ScenarioStructureBSP::struct_little *>(bsp_struct.data.data());
        
        // Go through all of the lightmap materials now?
        std::size_t lightmap_count = bsp_data.lightmaps.count;
        
        struct LightmapMaterialTemp {
            BuildWorkload::BuildWorkloadStruct *material_struct;
            Parser::ScenarioStructureBSPMaterial::struct_little *material;
        };
        std::vector<LightmapMaterialTemp> lightmap_materials; // hold the struct index and an offset
        
        if(lightmap_count > 0) {
            auto &lightmap_struct = workload.structs[*bsp_struct.resolve_pointer(&bsp_data.lightmaps.pointer)];
            auto *lightmap_data = reinterpret_cast<Parser::ScenarioStructureBSPLightmap::struct_little *>(lightmap_struct.data.data());
            for(std::size_t lm = 0; lm < lightmap_count; lm++) {
                auto &lightmap = lightmap_data[lm];
                std::size_t material_count = lightmap.materials.count;
                
                if(material_count > 0) {
                    auto &materials_struct = workload.structs[*lightmap_struct.resolve_pointer(&lightmap.materials.pointer)];
                    auto *materials = reinterpret_cast<Parser::ScenarioStructureBSPMaterial::struct_little *>(materials_struct.data.data());
                    
                    for(std::size_t mat = 0; mat < material_count; mat++) {
                        lightmap_materials.emplace_back(LightmapMaterialTemp { &materials_struct, materials + mat });
                    }
                }
            }
        }
        
        // Add these things
        struct MemeBSPPointer {
            PAD(4);
            HEK::LittleEndian<HEK::Pointer> pointer;
            PAD(4);
        };
        
        // Add vertices/indices pointers
        auto &rendered_vertices_ptr = bsp_header_struct.pointers.emplace_back();
        rendered_vertices_ptr.limit_to_32_bits = true;
        rendered_vertices_ptr.struct_index = rendered_vertices_struct_index;
        rendered_vertices_ptr.offset = reinterpret_cast<std::byte *>(&bsp_header.rendered_vertices) - reinterpret_cast<std::byte *>(&bsp_header);
        
        auto &lightmap_vertices_ptr = bsp_header_struct.pointers.emplace_back();
        lightmap_vertices_ptr.limit_to_32_bits = true;
        lightmap_vertices_ptr.struct_index = lightmap_vertices_struct_index;
        lightmap_vertices_ptr.offset = reinterpret_cast<std::byte *>(&bsp_header.lightmap_vertices) - reinterpret_cast<std::byte *>(&bsp_header);
        
        auto &rendered_vertices_struct = workload.structs[rendered_vertices_struct_index];
        rendered_vertices_struct.bsp = bsp;
        
        auto &lightmap_vertices_struct = workload.structs[lightmap_vertices_struct_index];
        lightmap_vertices_struct.bsp = bsp;
        
        std::size_t material_count = lightmap_materials.size();
        bsp_header.lightmap_material_count = material_count;
        bsp_header.lightmap_material_count_again = material_count;
        
        rendered_vertices_struct.data.resize(sizeof(MemeBSPPointer) * material_count);
        auto *rendered_pointers = reinterpret_cast<MemeBSPPointer *>(rendered_vertices_struct.data.data());
        lightmap_vertices_struct.data.resize(sizeof(MemeBSPPointer) * material_count);
        auto *lightmap_pointers = reinterpret_cast<MemeBSPPointer *>(lightmap_vertices_struct.data.data());
        
        for(std::size_t m = 0; m < material_count; m++) {
            auto &rv = rendered_pointers[m];
            auto &lm = lightmap_pointers[m];
            auto &mat = lightmap_materials[m];
            auto cv = mat.material_struct->resolve_pointer(&mat.material->compressed_vertices.pointer).value();
            
            auto &rvp = rendered_vertices_struct.pointers.emplace_back();
            rvp.limit_to_32_bits = true;
            rvp.offset = reinterpret_cast<std::byte *>(&rv.pointer) - reinterpret_cast<std::byte *>(rendered_pointers);
            rvp.struct_index = cv;
            
            auto &lmp = lightmap_vertices_struct.pointers.emplace_back();
            lmp.limit_to_32_bits = true;
            lmp.offset = reinterpret_cast<std::byte *>(&lm.pointer) - reinterpret_cast<std::byte *>(lightmap_pointers);
            lmp.struct_index = cv;
            lmp.struct_data_offset = mat.material->rendered_vertices_offset;
            
            auto &rvp_from_material = mat.material_struct->pointers.emplace_back();
            rvp_from_material.limit_to_32_bits = true;
            rvp_from_material.offset = reinterpret_cast<std::byte *>(&mat.material->rendered_vertices_index_pointer) - mat.material_struct->data.data();
            rvp_from_material.struct_index = rendered_vertices_struct_index;
            rvp_from_material.struct_data_offset = reinterpret_cast<std::byte *>(&rv) - reinterpret_cast<std::byte *>(rendered_pointers);
            
            auto &lmp_from_material = mat.material_struct->pointers.emplace_back();
            lmp_from_material.limit_to_32_bits = true;
            lmp_from_material.offset = reinterpret_cast<std::byte *>(&mat.material->lightmap_vertices_index_pointer) - mat.material_struct->data.data();
            lmp_from_material.struct_index = lightmap_vertices_struct_index;
            lmp_from_material.struct_data_offset = reinterpret_cast<std::byte *>(&lm) - reinterpret_cast<std::byte *>(lightmap_pointers);
        }
    }
}
