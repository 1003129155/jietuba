/// 图像拼接模块 - 长截图拼接核心逻辑
///
/// 包含：
/// - 智能拼接 (stitch_two_images_smart) - 多候选纠错
/// - 自动方向检测拼接 (stitch_two_images_smart_auto) - 自动检测正/反向滚动

use image::{DynamicImage, GenericImageView, ImageBuffer, Rgba};
use std::io::Cursor;

use crate::hash::compute_row_hashes_from_rgba;
use crate::lcs::find_top_common_substrings;

// ========== 内部工具函数 ==========

/// 从哈希序列中智能选择最佳候选
///
/// 返回 (start_i_abs, start_j, overlap_length)，如果无候选返回 Err
fn select_best_candidate(
    candidates: &[(i32, i32, usize)],
    search_start: usize,
    img1_len: usize,
    img2_len: usize,
    debug: bool,
) -> Result<(i32, i32, usize), String> {
    if candidates.is_empty() {
        if debug {
            println!("  ❌ 未找到任何重叠区域");
        }
        return Err("No overlap found".to_string());
    }

    if debug {
        println!("  🔍 找到 {} 个候选子串", candidates.len());
    }

    let mut best_candidate: Option<(i32, i32, usize)> = None;
    let longest_len = candidates[0].2;

    for (idx, &(relative_start_i, start_j, overlap_length)) in candidates.iter().enumerate() {
        let start_i = (relative_start_i + search_start as i32) as usize;
        let overlap_ratio = overlap_length as f32 / img1_len.min(img2_len) as f32;

        let img1_keep_height = start_i + overlap_length;
        let img2_skip_height = start_j as usize + overlap_length;
        let img2_keep_height = img2_len.saturating_sub(img2_skip_height);
        let result_height = img1_keep_height + img2_keep_height;

        let will_shrink = result_height < img1_len;

        if debug {
            println!(
                "\n  📌 候选 #{}: 长度{}行, 占比{:.2}%",
                idx + 1,
                overlap_length,
                overlap_ratio * 100.0
            );
            println!(
                "     位置: img1[{}:{}] ↔ img2[{}:{}]",
                start_i,
                start_i + overlap_length,
                start_j,
                start_j as usize + overlap_length
            );
            println!(
                "     预测结果: {}行 -> {}行 {}",
                img1_len,
                result_height,
                if will_shrink {
                    format!("❌ (减少{}行)", img1_len - result_height)
                } else {
                    format!("✅ (增加{}行)", result_height - img1_len)
                }
            );

            if will_shrink {
                println!(
                    "     img1保留{}行, 丢弃底部{}行",
                    img1_keep_height,
                    img1_len - img1_keep_height
                );
                println!("     img2新增{}行, 无法弥补损失", img2_keep_height);
            }
        }

        if !will_shrink {
            if longest_len > overlap_length * 5 {
                if debug {
                    println!("  ⚠️  跳过: 匹配长度{}远小于最长候选{}，疑似噪声", overlap_length, longest_len);
                }
                continue;
            }
            best_candidate = Some((start_i as i32, start_j, overlap_length));
            if debug {
                println!("  ✅ 选择此候选作为最佳匹配");
            }
            break;
        }
    }

    // 如果没有合适的不缩短候选，使用最长候选（回滚场景）
    let result = match best_candidate {
        Some(c) => c,
        None => {
            if debug {
                println!("\n  🔄 无可信的非缩短候选，使用最长匹配（可能是回滚场景）");
            }
            let first = &candidates[0];
            ((first.0 + search_start as i32), first.1, first.2)
        }
    };

    Ok(result)
}

/// 用候选参数执行实际的像素拼接
///
/// 返回 RGBA 字节 + 宽高
fn do_pixel_stitch(
    img1_rgba: &image::RgbaImage,
    img2_rgba: &image::RgbaImage,
    final_width: u32,
    height2: u32,
    start_i: i32,
    start_j: i32,
    overlap_length: usize,
    debug: bool,
) -> (Vec<u8>, u32, u32) {
    let img1_keep_height = (start_i as usize + overlap_length) as u32;
    let img2_skip_height = (start_j as usize + overlap_length) as u32;
    let img2_keep_height = height2.saturating_sub(img2_skip_height);
    let result_height = img1_keep_height + img2_keep_height;

    if debug {
        println!(
            "\n拼接计算: img1保留{}行 + img2跳过{}行保留{}行 = 总计{}行",
            img1_keep_height, img2_skip_height, img2_keep_height, result_height
        );
    }

    let row_bytes = (final_width * 4) as usize;
    let mut result_buf: Vec<u8> = vec![0u8; row_bytes * result_height as usize];

    let img1_raw = img1_rgba.as_raw();
    for y in 0..img1_keep_height as usize {
        let dst_start = y * row_bytes;
        let src_start = y * row_bytes;
        result_buf[dst_start..dst_start + row_bytes]
            .copy_from_slice(&img1_raw[src_start..src_start + row_bytes]);
    }

    let img2_raw = img2_rgba.as_raw();
    for y in 0..img2_keep_height as usize {
        let dst_start = (y + img1_keep_height as usize) * row_bytes;
        let src_start = (y + img2_skip_height as usize) * row_bytes;
        result_buf[dst_start..dst_start + row_bytes]
            .copy_from_slice(&img2_raw[src_start..src_start + row_bytes]);
    }

    (result_buf, final_width, result_height)
}

/// RGBA 字节编码为 PNG
fn encode_png(rgba_buf: Vec<u8>, width: u32, height: u32) -> Result<Vec<u8>, String> {
    let result: ImageBuffer<Rgba<u8>, Vec<u8>> =
        ImageBuffer::from_raw(width, height, rgba_buf)
            .ok_or_else(|| "Failed to create result image buffer".to_string())?;
    let mut output = Vec::new();
    DynamicImage::ImageRgba8(result)
        .write_to(&mut Cursor::new(&mut output), image::ImageOutputFormat::Png)
        .map_err(|e| format!("Failed to encode result: {}", e))?;
    Ok(output)
}

/// 从两张 RgbaImage 执行智能拼接的核心逻辑
///
/// 返回 (rgba_bytes, width, height)
fn smart_stitch_core(
    img1_rgba: &image::RgbaImage,
    img2_rgba: &image::RgbaImage,
    final_width: u32,
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
    debug: bool,
) -> Result<(Vec<u8>, u32, u32), String> {
    let height2 = img2_rgba.height();

    if debug {
        println!("忽略右侧 {} 像素来排除滚动条影响", ignore_right_pixels);
    }

    // 计算行哈希
    let img1_hashes = compute_row_hashes_from_rgba(img1_rgba, ignore_right_pixels, debug);
    let img2_hashes = compute_row_hashes_from_rgba(img2_rgba, ignore_right_pixels, debug);

    // 搜索区域设置（2倍窗口，容忍回滚）
    let img1_len = img1_hashes.len();
    let img2_len = img2_hashes.len();
    let search_window = img2_len * 2;
    let search_start = if img1_len > search_window {
        img1_len - search_window
    } else {
        0
    };
    let img1_search_region = &img1_hashes[search_start..];

    if debug {
        println!("  🔍 搜索重叠区域:");
        println!("     img1总长度: {}行", img1_len);
        println!("     img2总长度: {}行", img2_len);
        println!(
            "     搜索范围: img1[{}:{}] (底部{}行)",
            search_start, img1_len, img1_search_region.len()
        );
    }

    // 找多个候选子串
    let candidates = find_top_common_substrings(
        img1_search_region,
        &img2_hashes,
        min_overlap_ratio,
        5,
    );

    // 智能选择
    let (start_i, start_j, overlap_length) = select_best_candidate(
        &candidates,
        search_start,
        img1_len,
        img2_len,
        debug,
    )?;

    // 执行像素拼接
    Ok(do_pixel_stitch(
        img1_rgba, img2_rgba, final_width, height2,
        start_i, start_j, overlap_length, debug,
    ))
}

// ========== 公开 API ==========

/// 智能双图拼接（PNG 接口）
pub fn stitch_two_images_smart(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_smart_internal(img1_bytes, img2_bytes, ignore_right_pixels, min_overlap_ratio, false)
}

/// 智能双图拼接（调试模式）
pub fn stitch_two_images_smart_debug(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_smart_internal(img1_bytes, img2_bytes, ignore_right_pixels, min_overlap_ratio, true)
}

fn stitch_two_images_smart_internal(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
    debug: bool,
) -> Result<Vec<u8>, String> {
    // 加载图片
    let mut img1 = image::load_from_memory(img1_bytes)
        .map_err(|e| format!("Failed to load image 1: {}", e))?;
    let img2 = image::load_from_memory(img2_bytes)
        .map_err(|e| format!("Failed to load image 2: {}", e))?;

    let (width1, height1) = img1.dimensions();
    let (width2, height2) = img2.dimensions();

    if debug {
        println!("处理图片: ({}, {}) + ({}, {})", width1, height1, width2, height2);
    }

    // 宽度对齐
    if width1 != width2 {
        if debug { println!("调整图片宽度: {} -> {}", width1, width2); }
        let new_height1 = (height1 as f32 * width2 as f32 / width1 as f32) as u32;
        img1 = img1.resize_exact(width2, new_height1, image::imageops::FilterType::Lanczos3);
    }

    let (final_width, _) = img1.dimensions();
    let img1_rgba = img1.to_rgba8();
    let img2_rgba = img2.to_rgba8();

    let (result_buf, w, h) = smart_stitch_core(
        &img1_rgba, &img2_rgba, final_width,
        ignore_right_pixels, min_overlap_ratio, debug,
    )?;

    encode_png(result_buf, w, h)
}

/// 智能拼接 + 自动方向检测（PNG 接口）
///
/// 功能：
/// 1. 先正向拼接
/// 2. 如果正向失败或结果缩短 → 翻转图片重试
/// 3. 比较两个方向的结果，选更好的
///
/// 返回: (png_bytes, direction)
///   direction: "forward" 或 "reverse"
///
/// ⚠️  重要约定：
///   - "forward" → 返回正常朝向的拼接结果
///   - "reverse" → 返回**翻转态**的拼接结果（即对翻转后的图片拼接的产物）
///     调用方约定将翻转态结果存储，在最终输出时再翻转还原。
///     不在此处翻转回来，是为了与 Python 端 stitched_result 的"翻转态"约定保持一致。
pub fn stitch_two_images_smart_auto(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<(Vec<u8>, String), String> {
    stitch_two_images_smart_auto_internal(
        img1_bytes, img2_bytes, ignore_right_pixels, min_overlap_ratio, false,
    )
}

/// 自动方向检测（调试模式）
pub fn stitch_two_images_smart_auto_debug(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<(Vec<u8>, String), String> {
    stitch_two_images_smart_auto_internal(
        img1_bytes, img2_bytes, ignore_right_pixels, min_overlap_ratio, true,
    )
}

fn stitch_two_images_smart_auto_internal(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
    debug: bool,
) -> Result<(Vec<u8>, String), String> {
    // 加载图片
    let mut img1 = image::load_from_memory(img1_bytes)
        .map_err(|e| format!("Failed to load image 1: {}", e))?;
    let img2 = image::load_from_memory(img2_bytes)
        .map_err(|e| format!("Failed to load image 2: {}", e))?;

    let (width1, height1) = img1.dimensions();
    let (width2, height2) = img2.dimensions();

    if debug {
        println!("处理图片: ({}, {}) + ({}, {})", width1, height1, width2, height2);
    }

    // 宽度对齐
    if width1 != width2 {
        if debug { println!("调整图片宽度: {} -> {}", width1, width2); }
        let new_height1 = (height1 as f32 * width2 as f32 / width1 as f32) as u32;
        img1 = img1.resize_exact(width2, new_height1, image::imageops::FilterType::Lanczos3);
    }

    let (final_width, _) = img1.dimensions();
    let img1_rgba = img1.to_rgba8();
    let img2_rgba = img2.to_rgba8();
    let img1_h = img1_rgba.height();

    // ===== 1. 正向尝试 =====
    if debug {
        println!("\n━━━ 正向拼接尝试 ━━━");
    }

    let forward_result = smart_stitch_core(
        &img1_rgba, &img2_rgba, final_width,
        ignore_right_pixels, min_overlap_ratio, debug,
    );

    let forward_ok = match &forward_result {
        Ok((_, _, h)) => *h >= img1_h as u32,  // 没有缩短
        Err(_) => false,
    };

    if forward_ok {
        // 正向拼接成功且没缩短，直接使用
        let (buf, w, h) = forward_result.unwrap();
        if debug {
            println!("✅ 正向拼接成功 ({}行 → {}行)", img1_h, h);
        }
        let png = encode_png(buf, w, h)?;
        return Ok((png, "forward".to_string()));
    }

    // ===== 2. 正向失败或缩短，翻转重试 =====
    if debug {
        match &forward_result {
            Ok((_, _, h)) => println!("\n⚠️  正向拼接结果缩短 ({}行 → {}行)，尝试反向...", img1_h, h),
            Err(e) => println!("\n⚠️  正向拼接失败 ({})，尝试反向...", e),
        }
        println!("\n━━━ 反向拼接尝试（翻转哈希数组）━━━");
    }

    // 翻转行哈希 = 垂直翻转图片（但不需要真的翻转像素，只翻转哈希序列即可做匹配）
    // 但是像素拼接时需要真的翻转图片，所以这里还是翻转 RgbaImage
    let img1_flipped = image::imageops::flip_vertical(&img1_rgba);
    let img2_flipped = image::imageops::flip_vertical(&img2_rgba);

    let reverse_result = smart_stitch_core(
        &img1_flipped, &img2_flipped, final_width,
        ignore_right_pixels, min_overlap_ratio, debug,
    );

    // ===== 3. 比较正/反向结果 =====
    match (&forward_result, &reverse_result) {
        (_, Ok((rev_buf, rev_w, rev_h))) => {
            let rev_h_val = *rev_h;
            // 反向成功
            if rev_h_val >= img1_h as u32 {
                // 反向不缩短 → 使用反向（保持翻转态，不翻转回来）
                if debug {
                    println!("✅ 反向拼接成功 ({}行 → {}行)，检测到反向滚动", img1_h, rev_h_val);
                    println!("   返回翻转态结果（调用方负责最终输出时翻转还原）");
                }
                let png = encode_png(rev_buf.clone(), *rev_w, rev_h_val)?;
                return Ok((png, "reverse".to_string()));
            }

            // 反向也缩短了，跟正向比，选更好的
            match &forward_result {
                Ok((fwd_buf, fwd_w, fwd_h)) => {
                    if rev_h_val > *fwd_h {
                        if debug {
                            println!("🔄 两个方向都缩短，反向较优 (正向{}行 vs 反向{}行)", fwd_h, rev_h_val);
                        }
                        // 反向较优，返回翻转态
                        let png = encode_png(rev_buf.clone(), *rev_w, rev_h_val)?;
                        return Ok((png, "reverse".to_string()));
                    } else {
                        if debug {
                            println!("🔄 两个方向都缩短，正向较优 (正向{}行 vs 反向{}行)", fwd_h, rev_h_val);
                        }
                        let png = encode_png(fwd_buf.clone(), *fwd_w, *fwd_h)?;
                        return Ok((png, "forward".to_string()));
                    }
                }
                Err(_) => {
                    // 正向失败，反向虽然缩短但有结果，返回翻转态
                    if debug {
                        println!("⚠️  正向失败，使用反向结果（虽然缩短，返回翻转态）");
                    }
                    let png = encode_png(rev_buf.clone(), *rev_w, rev_h_val)?;
                    return Ok((png, "reverse".to_string()));
                }
            }
        }
        (Ok((fwd_buf, fwd_w, fwd_h)), Err(_)) => {
            // 反向失败，正向虽然缩短但有结果
            if debug {
                println!("⚠️  反向失败，使用正向结果（虽然缩短）");
            }
            let png = encode_png(fwd_buf.clone(), *fwd_w, *fwd_h)?;
            return Ok((png, "forward".to_string()));
        }
        (Err(e1), Err(e2)) => {
            // 两个方向都失败
            return Err(format!("Both directions failed: forward={}, reverse={}", e1, e2));
        }
    }
}
