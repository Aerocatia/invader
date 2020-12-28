// SPDX-License-Identifier: GPL-3.0-only

#include <invader/tag/parser/parser.hpp>
#include <invader/tag/hek/class/bitmap.hpp>
#include <invader/build/build_workload.hpp>
#include <invader/bitmap/swizzle.hpp>

namespace Invader::Parser {
    template <typename T> static bool power_of_two(T value) {
        while(value > 1) {
            if(value & 1) {
                return false;
            }
            value >>= 1;
        }
        return value & 1;
    }

    static std::size_t size_of_bitmap(const BitmapData &data) {
        std::size_t size = 0;
        std::size_t bits_per_pixel = HEK::calculate_bits_per_pixel(data.format);

        // Is this a meme?
        if(bits_per_pixel == 0) {
            eprintf_error("Unknown format %u", static_cast<unsigned int>(data.format));
            throw std::exception();
        }

        std::size_t height = data.height;
        std::size_t width = data.width;
        std::size_t depth = data.depth;
        bool should_be_compressed = data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
        std::size_t multiplier = data.type == HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP ? 6 : 1;
        std::size_t block_length = should_be_compressed ? 4 : 1;

        // Do it
        for(std::size_t i = 0; i <= data.mipmap_count; i++) {
            size += width * height * depth * multiplier * bits_per_pixel / 8;
            
            // Divide by 2, resetting back to 1 when needed, but make sure we don't go below the block length (4x4 if DXT, else 1x1)
            width = std::max(width / 2, block_length);
            height = std::max(height / 2, block_length);
            depth = std::max(depth / 2, static_cast<std::size_t>(1));
        }

        return size;
    }

    void BitmapData::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t struct_index, std::size_t offset) {
        auto &s = workload.structs[struct_index];
        auto *data = s.data.data();
        std::size_t bitmap_data_offset = reinterpret_cast<std::byte *>(&(reinterpret_cast<BitmapData::struct_little *>(data + offset)->bitmap_tag_id)) - data;
        this->pointer = 0xFFFFFFFF;
        this->flags |= HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_MAKE_IT_ACTUALLY_WORK;

        // Add itself as a dependency. I don't know why but apparently we need to remind ourselves that we're still ourselves.
        auto &d = s.dependencies.emplace_back();
        d.tag_index = tag_index;
        d.offset = bitmap_data_offset;
        d.tag_id_only = true;
    }

    template <typename T> static void do_post_cache_parse(T *bitmap, const Invader::Tag &tag) {
        bitmap->postprocess_hek_data();

        auto &map = tag.get_map();
        auto engine = map.get_engine();
        auto xbox = engine == HEK::CacheFileEngine::CACHE_FILE_XBOX;
        auto &base_struct = tag.get_base_struct<HEK::Bitmap>();
        
        // Un-zero out these if we're sprites (again, this is completely *insane* but compiled maps have this zeroed out for whatever reason which can completely FUCK things up if this were to not be "sprites" all of a sudden)
        if(bitmap->type == HEK::BitmapType::BITMAP_TYPE_SPRITES) {
            for(auto &sequence : bitmap->bitmap_group_sequence) {
                // Default
                sequence.first_bitmap_index = NULL_INDEX;
                sequence.bitmap_count = sequence.sprites.size() == 1 ? 1 : 0; // set to 1 if we have one sprite; 0 otherwise
                
                // If we have sprites, find the lowest bitmap index of them all
                if(sequence.sprites.size() != 0) {
                    for(auto &sprite : sequence.sprites) {
                        sequence.first_bitmap_index = std::min(sequence.first_bitmap_index, sprite.bitmap_index);
                    }
                }
            }
        }

        // Do we have bitmap data?
        auto bd_count = bitmap->bitmap_data.size();
        if(bd_count) {
            auto *bitmap_data_le_array = tag.resolve_reflexive(base_struct.bitmap_data);
            
            for(std::size_t bd = 0; bd < bd_count; bd++) {
                auto &bitmap_data = bitmap->bitmap_data[bd];
                auto &bitmap_data_le = bitmap_data_le_array[bd];
                std::size_t size = bitmap_data.pixel_data_size;
                
                bool compressed = bitmap_data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
                bool should_be_compressed = false;
                
                switch(bitmap_data.format) {
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1:
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3:
                    case HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5:
                        should_be_compressed = true;
                        break;
                    default:
                        should_be_compressed = false;
                }
                
                // Bad!
                if(should_be_compressed != compressed) {
                    if(compressed) {
                        eprintf_error("Bitmap is incorrectly marked as compressed but is NOT DXT; tag is corrupt");
                    }
                    else {
                        eprintf_error("Bitmap is incorrectly NOT marked as compressed but is DXT; tag is corrupt");
                    }
                    throw InvalidTagDataException();
                }
                
                // Also check if it needs deswizzled (don't do it yet)
                bool swizzled = bitmap_data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                if(swizzled) {
                    if(compressed) {
                        eprintf_error("Bitmap is incorrectly marked as compressed AND swizzled; tag is corrupt");
                        throw InvalidTagDataException();
                    }
                }
                
                // Nope
                if(bitmap_data.depth != 1 && bitmap_data.type != HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE) {
                    eprintf_error("Bitmap has depth but is not a 3D texture");
                    throw InvalidTagDataException();
                }
                if(!power_of_two(bitmap_data.depth)) {
                    eprintf_error("Bitmap depth is non-power-of-two");
                    throw InvalidTagDataException();
                }
                
                // Get it!
                const std::byte *bitmap_data_ptr;
                if(bitmap_data_le.flags.read() & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_EXTERNAL) {
                    bitmap_data_ptr = map.get_data_at_offset(bitmap_data.pixel_data_offset, size, Map::DATA_MAP_BITMAP);
                }
                else {
                    bitmap_data_ptr = map.get_internal_asset(bitmap_data.pixel_data_offset, size);
                }
                
                // Xbox buffer (for Xbox bitmaps), since the size will always be a multiple of 128 and thus won't be the same size as a PC bitmap
                std::vector<std::byte> xbox_to_pc_buffer;
                
                if(xbox) {
                    // Set flag as unswizzled
                    bitmap_data.flags = bitmap_data.flags & ~HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                    
                    // Set our buffer up
                    xbox_to_pc_buffer.resize(size_of_bitmap(bitmap_data));
                    
                    auto copy_texture = [&xbox_to_pc_buffer, &swizzled, &compressed, &bitmap_data_ptr, &size, &bitmap_data](std::optional<std::size_t> input_offset = std::nullopt, std::optional<std::size_t> output_cubemap_face = std::nullopt) -> std::size_t {
                        std::size_t bits_per_pixel = calculate_bits_per_pixel(bitmap_data.format);
                        std::size_t real_mipmap_count = bitmap_data.mipmap_count;
                        std::size_t height = bitmap_data.height;
                        std::size_t width = bitmap_data.width;
                        std::size_t depth = bitmap_data.depth;
                        
                        auto *base_input = bitmap_data_ptr + input_offset.value_or(0);
                        auto *input = base_input;
                        
                        auto *output = xbox_to_pc_buffer.data();
                        
                        // Offset the output
                        if(output_cubemap_face.has_value()) {
                            output += (height * width * depth * bits_per_pixel) / 8 * *output_cubemap_face;
                        }
                        
                        std::size_t minimum_dimension;
                        std::size_t minimum_dimension_depth = 1;
                        
                        if(compressed) {
                            // Resolution the bitmap is stored as
                            if(height % 4) {
                                height += 4 - (height % 4);
                            }
                            if(width % 4) {
                                width += 4 - (width % 4);
                            }
                            
                            // Mipmaps less than 4x4 don't exist in Xbox maps
                            while((height >> real_mipmap_count) < 4 && (width >> real_mipmap_count) < 4 && real_mipmap_count > 0) {
                                real_mipmap_count--;
                            }
                            
                            minimum_dimension = 4;
                        }
                        else {
                            minimum_dimension = 1;
                        }
                        
                        std::size_t mipmap_width = width;
                        std::size_t mipmap_height = height;
                        std::size_t mipmap_depth = depth;
                        
                        // Copy this stuff
                        for(std::size_t m = 0; m <= real_mipmap_count; m++) {
                            std::size_t mipmap_size = mipmap_width * mipmap_height * mipmap_depth * bits_per_pixel / 8;
                            
                            // Bitmap data is out of bounds?
                            if((input - base_input) + mipmap_size > size) {
                                throw OutOfBoundsException();
                            }
                            
                            // Swizzle that stuff!
                            if(swizzled) {
                                auto swizzled_data = Invader::Swizzle::swizzle(input, bits_per_pixel, mipmap_width, mipmap_height, mipmap_depth, true);
                                std::memcpy(output, swizzled_data.data(), mipmap_size);
                            }
                            else {
                                std::memcpy(output, input, mipmap_size);
                            }
                            
                            // Continue...
                            std::size_t stride_count = output_cubemap_face.has_value() ? (6 - *output_cubemap_face) : 1;
                            output += stride_count * mipmap_size;
                            input += mipmap_size;
                            
                            // Halve the dimensions
                            mipmap_width = std::max(mipmap_width / 2, minimum_dimension);
                            mipmap_height = std::max(mipmap_height / 2, minimum_dimension);
                            mipmap_depth = std::max(mipmap_depth / 2, minimum_dimension_depth);
                            
                            // Skip that data, too
                            if(output_cubemap_face.has_value()) {
                                mipmap_size = mipmap_width * mipmap_height * mipmap_depth * bits_per_pixel / 8;
                                output += *output_cubemap_face * mipmap_size;
                            }
                        }
                        
                        if(compressed) {
                            // Get the block size
                            auto block_size = (minimum_dimension * minimum_dimension * bits_per_pixel) / 8;
                            
                            // Go back a block and just copy that
                            input -= block_size;
                            
                            // Lastly, copy the missing mipmaps if necessary
                            for(std::size_t m = real_mipmap_count; m < bitmap_data.mipmap_count; m++) {
                                std::memcpy(output, input, block_size);
                                std::size_t stride_count = output_cubemap_face.has_value() ? 6 : 1; // since all mipmaps from here on out are the same in size, we just need to add this once this time
                                output += stride_count * block_size;
                            }
                            
                            return (input - base_input) + block_size;
                        }
                        
                        else {
                            return input - base_input;
                        }
                    };
                    
                    auto copy_cube_map = [&copy_texture]() -> void {
                        std::size_t offset = 0;
                        for(std::size_t i = 0; i < 6; i++) {
                            int to_i;
                            if(i == 1) {
                                to_i = 2;
                            }
                            else if(i == 2) {
                                to_i = 1;
                            }
                            else {
                                to_i = i;
                            }
                            offset += copy_texture(offset, to_i);
                            offset += REQUIRED_PADDING_N_BYTES(offset, HEK::CacheFileXboxConstants::CACHE_FILE_XBOX_BITMAP_SIZE_GRANULARITY);
                        }
                    };
                    
                    switch(bitmap_data.type) {
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP:
                            copy_cube_map();
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE:
                            copy_texture();
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_WHITE:
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_2D_TEXTURE:
                            copy_texture();
                            break;
                        default:
                            throw std::exception();
                            break;
                    }
                    
                    
                    bitmap_data_ptr = xbox_to_pc_buffer.data();
                    size = xbox_to_pc_buffer.size();
                    bitmap_data.pixel_data_size = size;
                }

                // Calculate our offset
                bitmap_data.pixel_data_offset = static_cast<std::size_t>(bitmap->processed_pixel_data.size());
                
                // Insert this
                bitmap->processed_pixel_data.insert(bitmap->processed_pixel_data.end(), bitmap_data_ptr, bitmap_data_ptr + size);
            }
        }
    }

    template <typename T> static void do_pre_compile(T *bitmap, BuildWorkload &workload, std::size_t tag_index) {
        // Delete null group sequences at the end
        while(bitmap->bitmap_group_sequence.size() > 0 && bitmap->bitmap_group_sequence[bitmap->bitmap_group_sequence.size() - 1].first_bitmap_index == NULL_INDEX) {
            bitmap->bitmap_group_sequence.erase(bitmap->bitmap_group_sequence.begin() + (bitmap->bitmap_group_sequence.size() - 1));
        }
        
        // Zero out these if we're sprites (this is completely *insane* but that's what tool.exe does)
        if(bitmap->type == HEK::BitmapType::BITMAP_TYPE_SPRITES) {
            for(auto &sequence : bitmap->bitmap_group_sequence) {
                sequence.first_bitmap_index = 0;
                sequence.bitmap_count = 0;
            }
        }
        
        // Loop through again, but make sure sprites are present when needed and not present when not needed
        bool has_sprites = false;
        for(auto &sequence : bitmap->bitmap_group_sequence) {
            if((has_sprites = sequence.sprites.size() > 0)) {
                break;
            }
        }
        bool errored_on_sprites = false;
        if(has_sprites && bitmap->type != HEK::BitmapType::BITMAP_TYPE_SPRITES) {
            workload.report_error(BuildWorkload::ErrorType::ERROR_TYPE_FATAL_ERROR, "Bitmap has sprites but is not a sprites bitmap type", tag_index);
            errored_on_sprites = true;
        }
        else if(!has_sprites && bitmap->type == HEK::BitmapType::BITMAP_TYPE_SPRITES && bitmap->bitmap_data.size() > 0) {
            workload.report_error(BuildWorkload::ErrorType::ERROR_TYPE_FATAL_ERROR, "Bitmap with bitmap data is marked as sprites, but no sprites are present", tag_index);
            errored_on_sprites = true;
        }
        if(errored_on_sprites) {
            eprintf_warn("To fix this, recompile the bitmap");
            throw InvalidTagDataException();
        }
        
        auto max_size = bitmap->processed_pixel_data.size();
        auto *pixel_data = bitmap->processed_pixel_data.data();
        std::size_t bitmap_data_count = bitmap->bitmap_data.size();
        std::size_t swizzle_count = 0;
        const char *swizzle_verb = "";
        auto engine_target = workload.get_build_parameters()->details.build_cache_file_engine;
        
        for(std::size_t b = 0; b < bitmap_data_count; b++) {
            auto &data = bitmap->bitmap_data[b];
            bool swizzled = data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
            bool compressed = data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
            
            // DXTn bitmaps cannot be swizzled
            if(swizzled && compressed) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu is marked as compressed and swizzled which is not allowed", b);
                throw InvalidTagDataException();
            }
            
            // Should we (de)swizzle?
            auto do_swizzle = [&data, &pixel_data, &swizzled, &swizzle_verb, &swizzle_count](bool deswizzle) {
                std::size_t bits_per_pixel = HEK::calculate_bits_per_pixel(data.format);
                std::size_t mipmap_count = data.mipmap_count;
                std::size_t width = data.width;
                std::size_t height = data.height;
                std::size_t depth = data.depth;
                auto *this_bitmap_data = pixel_data + data.pixel_data_offset;
                
                // Go through each mipmap and insert them deswizzled
                for(std::size_t m = 0; m <= mipmap_count; m++) {
                    // Do it!
                    auto deswizzled = Invader::Swizzle::swizzle(this_bitmap_data, bits_per_pixel, width, height, depth, true);
                    auto deswizzled_size = deswizzled.size();
                    std::memcpy(this_bitmap_data, deswizzled.data(), deswizzled_size);
                    this_bitmap_data += deswizzled_size;
                    
                    // Make sure we don't go below 1x1
                    width = std::max(width / 2, static_cast<std::size_t>(1));
                    height = std::max(height / 2, static_cast<std::size_t>(1));
                    depth = std::max(depth / 2, static_cast<std::size_t>(1));
                }
                
                // Mark as (un)swizzled
                if(deswizzle) {
                    data.flags &= ~HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                    swizzled = false;
                    swizzle_verb = "deswizzled";
                }
                else {
                    data.flags |= HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_SWIZZLED;
                    swizzled = true;
                    swizzle_verb = "swizzled";
                }
                
                swizzle_count++;
            };

            // Check if we can or must use swizzled stuff
            switch(engine_target) {
                case HEK::CacheFileEngine::CACHE_FILE_DEMO:
                case HEK::CacheFileEngine::CACHE_FILE_RETAIL:
                case HEK::CacheFileEngine::CACHE_FILE_CUSTOM_EDITION:
                case HEK::CacheFileEngine::CACHE_FILE_NATIVE:
                    if(swizzled) {
                        do_swizzle(true);
                    }
                    break;
                case HEK::CacheFileEngine::CACHE_FILE_XBOX:
                    if(!compressed && !swizzled) {
                        do_swizzle(false);
                    }
                    break;
                default:
                    break;
            }

            std::size_t data_index = &data - bitmap->bitmap_data.data();
            auto format = data.format;
            auto type = data.type;
            bool should_be_compressed = (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1) || (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3) || (format == HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5);

            std::size_t depth = data.depth;
            std::size_t start = data.pixel_data_offset;
            std::size_t width = data.width;
            std::size_t height = data.height;
            
            auto *build_parameters = workload.get_build_parameters();

            // Warn for stuff
            if(build_parameters->verbosity > BuildWorkload::BuildParameters::BuildVerbosity::BUILD_VERBOSITY_HIDE_PEDANTIC) {
                bool exceeded = false;
                bool non_power_of_two = (!power_of_two(height) || !power_of_two(width) || !power_of_two(depth));

                if(
                    engine_target == HEK::CacheFileEngine::CACHE_FILE_CUSTOM_EDITION ||
                    engine_target == HEK::CacheFileEngine::CACHE_FILE_RETAIL ||
                    engine_target == HEK::CacheFileEngine::CACHE_FILE_DEMO
                ) {
                    if(bitmap->type != HEK::BitmapType::BITMAP_TYPE_INTERFACE_BITMAPS && non_power_of_two) {
                        REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Non-interface bitmap data #%zu is non-power-of-two (%zux%zux%zu)", data_index, width, height, depth);
                        exceeded = true;
                    }
                
                    switch(type) {
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_2D_TEXTURE:
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_WHITE:
                            if(width > 2048 || height > 2048) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 2048x2048 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE:
                            if(width > 256 || height > 256 || depth > 256) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 256x256x256 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_CUBE_MAP:
                            if(width > 512 || height > 512) {
                                 REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "Bitmap data #%zu exceeds 512x512 (%zux%zu)", data_index, width, height);
                                 exceeded = true;
                            }
                            break;
                        case HEK::BitmapDataType::BITMAP_DATA_TYPE_ENUM_COUNT:
                            break;
                    }
                    
                    if(exceeded) {
                        eprintf_warn("Target engine uses D3D9; some D3D9 compliant hardware may not render this bitmap");
                    }
                }
            }

            if(depth != 1 && type != HEK::BitmapDataType::BITMAP_DATA_TYPE_3D_TEXTURE) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu is not a 3D texture but has depth (%zu != 1)", data_index, depth);
                throw InvalidTagDataException();
            }

            // Make sure these are equal
            if(compressed != should_be_compressed) {
                const char *format_name = HEK::bitmap_data_format_name(format);
                data.flags ^= HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED; // invert the flag in case we need to do any math on it (though it's screwed up either way)
                if(compressed) {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu (format: %s) is incorrectly marked as compressed", data_index, format_name);
                    throw InvalidTagDataException();
                }
                else {
                    REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu (format: %s) is not marked as compressed", data_index, format_name);
                    throw InvalidTagDataException();
                }
            }

            auto size = size_of_bitmap(data);

            // Make sure we won't explode
            std::size_t end = start + size;
            if(start > max_size || size > max_size || end > max_size) {
                REPORT_ERROR_PRINTF(workload, ERROR_TYPE_FATAL_ERROR, tag_index, "Bitmap data #%zu range (0x%08zX - 0x%08zX) exceeds the processed pixel data size (0x%08zX)", data_index, start, end, max_size);
                throw InvalidTagDataException();
            }

            // Add it all
            std::size_t raw_data_index = workload.raw_data.size();
            workload.raw_data.emplace_back(pixel_data + start, pixel_data + end);
            workload.tags[tag_index].asset_data.emplace_back(raw_data_index);
            data.pixel_data_size = static_cast<std::uint32_t>(size);
        }
            
        // Indicate if we had to swizzle or deswizzle
        if(swizzle_count > 0) {
            REPORT_ERROR_PRINTF(workload, ERROR_TYPE_WARNING_PEDANTIC, tag_index, "%zu bitmap%s needed to be %s for the target engine", swizzle_count, swizzle_count == 1 ? "" : "s", swizzle_verb);
        }
    }

    template <typename T> static void do_postprocess_hek_data(T *bitmap) {
        if(bitmap->compressed_color_plate_data.size() == 0) {
            bitmap->color_plate_height = 0;
            bitmap->color_plate_width = 0;
        }
    }

    void Bitmap::postprocess_hek_data() {
        do_postprocess_hek_data(this);
    }

    void Bitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }

    void Bitmap::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t, std::size_t) {
        do_pre_compile(this, workload, tag_index);
    }

    void InvaderBitmap::postprocess_hek_data() {
        do_postprocess_hek_data(this);
    }

    void InvaderBitmap::post_cache_parse(const Invader::Tag &tag, std::optional<HEK::Pointer>) {
        do_post_cache_parse(this, tag);
    }

    void InvaderBitmap::pre_compile(BuildWorkload &workload, std::size_t tag_index, std::size_t, std::size_t) {
        do_pre_compile(this, workload, tag_index);
    }

    Bitmap downgrade_invader_bitmap(const InvaderBitmap &tag) {
        Bitmap new_tag = {};
        new_tag.type = tag.type;
        new_tag.encoding_format = tag.encoding_format;
        new_tag.usage = tag.usage;
        new_tag.flags = tag.flags;
        new_tag.detail_fade_factor = tag.detail_fade_factor;
        new_tag.sharpen_amount = tag.sharpen_amount;
        new_tag.bump_height = tag.bump_height;
        new_tag.sprite_budget_size = tag.sprite_budget_size;
        new_tag.sprite_budget_count = tag.sprite_budget_count;
        new_tag.color_plate_width = tag.color_plate_width;
        new_tag.color_plate_height = tag.color_plate_height;
        new_tag.compressed_color_plate_data = tag.compressed_color_plate_data;
        new_tag.processed_pixel_data = tag.processed_pixel_data;
        new_tag.blur_filter_size = tag.blur_filter_size;
        new_tag.alpha_bias = tag.alpha_bias;
        new_tag.mipmap_count = tag.mipmap_count;
        new_tag.sprite_usage = tag.sprite_usage;
        new_tag.sprite_spacing = tag.sprite_spacing;
        new_tag.bitmap_group_sequence = tag.bitmap_group_sequence;
        new_tag.bitmap_data = tag.bitmap_data;
        return new_tag;
    }

    template <typename T> static bool fix_power_of_two_for_tag(T &tag, bool fix) {
        bool fixed = false;
        for(auto &d : tag.bitmap_data) {
            fixed = fix_power_of_two(d, fix) || fixed;
            if(fixed && !fix) {
                return true;
            }
        }
        return fixed;
    }

    bool fix_power_of_two(InvaderBitmap &tag, bool fix) {
        return fix_power_of_two_for_tag(tag, fix);
    }

    bool fix_power_of_two(Bitmap &tag, bool fix) {
        return fix_power_of_two_for_tag(tag, fix);
    }

    bool fix_power_of_two(BitmapData &data, bool fix) {
        bool should_be_power_of_two = power_of_two(data.width) && power_of_two(data.height) && power_of_two(data.width);
        bool power_of_two_dimensions = data.flags & HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_POWER_OF_TWO_DIMENSIONS;
        
        if(power_of_two_dimensions && !should_be_power_of_two) {
            if(fix) {
                data.flags &= ~HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_POWER_OF_TWO_DIMENSIONS;
            }
            return true;
        }
        else if(!power_of_two_dimensions && should_be_power_of_two) {
            if(fix) {
                data.flags |= HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_POWER_OF_TWO_DIMENSIONS;
            }
            return true;
        }
        else {
            return false;
        }
    }
}
