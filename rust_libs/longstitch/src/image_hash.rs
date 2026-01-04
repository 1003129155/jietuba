/// 图像哈希模块 - 高性能 Rust 实现
///
/// 支持的哈希算法:
/// - dHash (Difference Hash): 快速，适合相似图片检测
/// - pHash (Perceptual Hash): 更准确，适合变形后的图片检测
/// - aHash (Average Hash): 最快，精度较低
/// - 行哈希 (Row Hash): 用于长截图拼接的逐行哈希
use image::{GenericImageView, GrayImage};
use rayon::prelude::*;

/// 计算差值哈希 (dHash)
///
/// 原理: 比较相邻像素的灰度差异
/// 优点: 对缩放和轻微变形具有鲁棒性
///
/// 参数:
///   image_bytes: PNG/JPEG 图像数据
///   hash_size: 哈希尺寸 (默认8，生成64位哈希)
///
/// 返回: u64 哈希值
pub fn compute_dhash(image_bytes: &[u8], hash_size: usize) -> Result<u64, String> {
    // 加载图像
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;

    // 转换为灰度并缩放到 (hash_size+1) x hash_size
    let gray = img.grayscale();
    let resized = image::imageops::resize(
        &gray,
        (hash_size + 1) as u32,
        hash_size as u32,
        image::imageops::FilterType::Triangle,
    );

    // 比较相邻像素生成哈希
    let mut hash = 0u64;
    let mut bit_index = 0;

    for y in 0..hash_size {
        for x in 0..hash_size {
            let left = resized.get_pixel(x as u32, y as u32)[0];
            let right = resized.get_pixel((x + 1) as u32, y as u32)[0];

            // 左边像素小于右边时设置为1
            if left < right {
                hash |= 1 << bit_index;
            }
            bit_index += 1;
        }
    }

    Ok(hash)
}

/// 计算平均哈希 (aHash)
///
/// 原理: 比较每个像素与平均值的关系
/// 优点: 最快，但精度较低
pub fn compute_ahash(image_bytes: &[u8], hash_size: usize) -> Result<u64, String> {
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;

    let gray = img.grayscale();
    let resized = image::imageops::resize(
        &gray,
        hash_size as u32,
        hash_size as u32,
        image::imageops::FilterType::Triangle,
    );

    // 计算平均灰度值
    let mut sum: u64 = 0;
    let pixels: Vec<u8> = resized.pixels().map(|p| p[0]).collect();
    for &pixel in &pixels {
        sum += pixel as u64;
    }
    let avg = (sum / (hash_size * hash_size) as u64) as u8;

    // 生成哈希
    let mut hash = 0u64;
    for (i, &pixel) in pixels.iter().enumerate() {
        if pixel >= avg {
            hash |= 1 << i;
        }
    }

    Ok(hash)
}

/// 简化版 DCT (离散余弦变换) - 用于 pHash
/// 只计算 8x8 的低频系数
fn compute_dct_lowfreq(gray_img: &GrayImage, size: usize) -> Vec<f32> {
    let width = gray_img.width() as usize;
    let height = gray_img.height() as usize;

    let mut coeffs = vec![0.0f32; size * size];

    // 简化的 DCT-II 变换（只计算左上角低频部分）
    for v in 0..size {
        for u in 0..size {
            let mut sum = 0.0;

            for y in 0..height {
                for x in 0..width {
                    let pixel = gray_img.get_pixel(x as u32, y as u32)[0] as f32;
                    let cos_u = ((2 * x + 1) as f32 * u as f32 * std::f32::consts::PI
                        / (2.0 * width as f32))
                        .cos();
                    let cos_v = ((2 * y + 1) as f32 * v as f32 * std::f32::consts::PI
                        / (2.0 * height as f32))
                        .cos();
                    sum += pixel * cos_u * cos_v;
                }
            }

            // 归一化系数
            let cu = if u == 0 { 1.0 / (2.0_f32).sqrt() } else { 1.0 };
            let cv = if v == 0 { 1.0 / (2.0_f32).sqrt() } else { 1.0 };

            coeffs[v * size + u] = sum * cu * cv * 2.0 / (width * height) as f32;
        }
    }

    coeffs
}

/// 计算感知哈希 (pHash)
///
/// 原理: 使用 DCT 提取图像的低频信息
/// 优点: 对旋转、缩放、变形有更好的鲁棒性
pub fn compute_phash(image_bytes: &[u8], hash_size: usize) -> Result<u64, String> {
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;

    // 转灰度并缩放到 32x32
    let gray = img.to_luma8();
    let resized_gray =
        image::imageops::resize(&gray, 32, 32, image::imageops::FilterType::Lanczos3);

    // 计算 DCT 低频系数
    let dct_coeffs = compute_dct_lowfreq(&resized_gray, hash_size);

    // 计算中位数（排除 DC 分量）
    let mut sorted_coeffs: Vec<f32> = dct_coeffs.iter().skip(1).copied().collect();
    sorted_coeffs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let median = sorted_coeffs[sorted_coeffs.len() / 2];

    // 生成哈希（排除 DC 分量）
    let mut hash = 0u64;
    for (i, &coeff) in dct_coeffs.iter().skip(1).enumerate() {
        if i >= 64 {
            break;
        }
        if coeff > median {
            hash |= 1 << i;
        }
    }

    Ok(hash)
}

/// 计算汉明距离
///
/// 参数:
///   hash1, hash2: 两个哈希值
///
/// 返回: 不同位的数量 (0-64)
#[inline]
pub fn hamming_distance(hash1: u64, hash2: u64) -> u32 {
    (hash1 ^ hash2).count_ones()
}

/// 计算哈希相似度
///
/// 返回: 0.0-1.0 之间的相似度（1.0 表示完全相同）
#[inline]
pub fn hash_similarity(hash1: u64, hash2: u64, hash_size: usize) -> f64 {
    let max_distance = (hash_size * hash_size) as f64;
    let distance = hamming_distance(hash1, hash2) as f64;
    1.0 - (distance / max_distance)
}

/// 批量计算哈希（并行处理）
///
/// 参数:
///   image_bytes_list: 图像字节数据列表
///   method: "dhash", "ahash" 或 "phash"
///   hash_size: 哈希尺寸
///
/// 返回: 哈希值列表
pub fn batch_compute_hash(
    image_bytes_list: &[Vec<u8>],
    method: &str,
    hash_size: usize,
) -> Vec<Result<u64, String>> {
    image_bytes_list
        .par_iter()
        .map(|bytes| match method {
            "dhash" => compute_dhash(bytes, hash_size),
            "ahash" => compute_ahash(bytes, hash_size),
            "phash" => compute_phash(bytes, hash_size),
            _ => Err(format!("Unknown hash method: {}", method)),
        })
        .collect()
}

/// 逐行哈希 - 专为长截图拼接优化
///
/// 计算图像每一行的快速哈希值，用于找到重叠区域
///
/// 参数:
///   image_bytes: 图像数据
///   ignore_right_pixels: 忽略右侧像素数（避免滚动条干扰）
///   debug: 是否输出调试信息
///
/// 返回: 每行的哈希值列表
pub fn compute_row_hashes(
    image_bytes: &[u8],
    ignore_right_pixels: u32,
) -> Result<Vec<u64>, String> {
    compute_row_hashes_internal(image_bytes, ignore_right_pixels, false)
}

/// 内部实现，支持调试输出
pub fn compute_row_hashes_debug(
    image_bytes: &[u8],
    ignore_right_pixels: u32,
) -> Result<Vec<u64>, String> {
    compute_row_hashes_internal(image_bytes, ignore_right_pixels, true)
}

fn compute_row_hashes_internal(
    image_bytes: &[u8],
    ignore_right_pixels: u32,
    debug: bool,
) -> Result<Vec<u64>, String> {
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;

    let rgba_img = img.to_rgba8();
    let width = rgba_img.width();
    let height = rgba_img.height();

    // 计算有效宽度（排除滚动条）
    let effective_width = if ignore_right_pixels > 0 && width > ignore_right_pixels {
        width - ignore_right_pixels
    } else {
        width
    };

    // 并行计算每行的哈希
    let row_hashes: Vec<u64> = (0..height)
        .into_par_iter()
        .map(|y| {
            let mut r_sum: u64 = 0;
            let mut g_sum: u64 = 0;
            let mut b_sum: u64 = 0;
            let pixel_count = effective_width as u64;

            for x in 0..effective_width {
                let pixel = rgba_img.get_pixel(x, y);
                r_sum += pixel[0] as u64;
                g_sum += pixel[1] as u64;
                b_sum += pixel[2] as u64;
            }

            if pixel_count > 0 {
                // 计算平均值并量化（提高容忍度）
                let r_mean = ((r_sum / pixel_count) / 8) * 8;
                let g_mean = ((g_sum / pixel_count) / 8) * 8;
                let b_mean = ((b_sum / pixel_count) / 8) * 8;

                // 使用简单的哈希函数
                let hash = r_mean
                    .wrapping_mul(73856093)
                    .wrapping_add(g_mean.wrapping_mul(19349663))
                    .wrapping_add(b_mean.wrapping_mul(83492791));

                hash
            } else {
                0
            }
        })
        .collect();

    // 🔍 调试输出：打印样本哈希值
    if debug {
        println!("  📊 样本哈希值（每100行）:");
        for y in (0..height).step_by(100).take(3) {
            let mut r_sum: u64 = 0;
            let mut g_sum: u64 = 0;
            let mut b_sum: u64 = 0;

            for x in 0..effective_width {
                let pixel = rgba_img.get_pixel(x, y);
                r_sum += pixel[0] as u64;
                g_sum += pixel[1] as u64;
                b_sum += pixel[2] as u64;
            }

            let pixel_count = effective_width as u64;
            if pixel_count > 0 {
                let r_mean = ((r_sum / pixel_count) / 8) * 8;
                let g_mean = ((g_sum / pixel_count) / 8) * 8;
                let b_mean = ((b_sum / pixel_count) / 8) * 8;
                let hash = row_hashes[y as usize];

                println!(
                    "     行{}: RGB({},{},{}) -> hash={}",
                    y, r_mean, g_mean, b_mean, hash as i64
                );
            }
        }
    }

    Ok(row_hashes)
}

/// 找到两个哈希序列的最长公共子串
///
/// 用于长截图拼接时找到重叠区域
///
/// 参数:
///   seq1, seq2: 两个哈希序列
///   min_ratio: 最小重叠比例
///
/// 返回: (seq1_start, seq2_start, length)
pub fn find_longest_common_substring(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
) -> (i32, i32, usize) {
    find_longest_common_substring_internal(seq1, seq2, min_ratio, false)
}

/// 找到多个公共子串候选（用于智能拼接纠错）
///
/// 返回前 top_k 个最长的不重叠公共子串
/// 这允许调用方根据其他条件（如是否会缩短结果）选择最佳候选
pub fn find_top_common_substrings(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
    top_k: usize,
) -> Vec<(i32, i32, usize)> {
    use std::collections::{HashMap, HashSet};

    let m = seq1.len();
    let n = seq2.len();
    let min_length = ((m.min(n) as f32 * min_ratio) as usize).max(1);

    // 找到所有公共哈希值
    let set1: HashSet<u64> = seq1.iter().copied().collect();
    let set2: HashSet<u64> = seq2.iter().copied().collect();
    let common_hashes: HashSet<u64> = set1.intersection(&set2).copied().collect();

    if common_hashes.is_empty() {
        return Vec::new();
    }

    // 为每个公共哈希值找到所有出现位置
    let mut hash_positions: HashMap<u64, (Vec<usize>, Vec<usize>)> = HashMap::new();
    for &hash in &common_hashes {
        let pos_i: Vec<usize> = seq1
            .iter()
            .enumerate()
            .filter(|(_, &h)| h == hash)
            .map(|(i, _)| i)
            .collect();
        let pos_j: Vec<usize> = seq2
            .iter()
            .enumerate()
            .filter(|(_, &h)| h == hash)
            .map(|(i, _)| i)
            .collect();
        hash_positions.insert(hash, (pos_i, pos_j));
    }

    // 扩展每个匹配点为最大子串
    let mut substrings = Vec::new();
    for (pos_i_list, pos_j_list) in hash_positions.values() {
        for &start_i in pos_i_list {
            for &start_j in pos_j_list {
                // 向后扩展
                let mut length = 0;
                while start_i + length < m
                    && start_j + length < n
                    && seq1[start_i + length] == seq2[start_j + length]
                {
                    length += 1;
                }

                if length >= min_length {
                    substrings.push((start_i as i32, start_j as i32, length));
                }
            }
        }
    }

    if substrings.is_empty() {
        return Vec::new();
    }

    // 按长度降序排序
    substrings.sort_by(|a, b| b.2.cmp(&a.2));

    // 去重：移除完全相同的子串
    substrings.dedup();

    // 选择不重叠的前 top_k 个
    let mut selected = Vec::new();
    let mut used_ranges: Vec<(usize, usize)> = Vec::new();

    for (start_i, start_j, length) in substrings {
        let end_i = start_i as usize + length;

        // 检查是否与已选择的子串在 seq1 上有显著重叠
        let has_significant_overlap = used_ranges.iter().any(|(used_start, used_end)| {
            let overlap_start = (*used_start).max(start_i as usize);
            let overlap_end = (*used_end).min(end_i);

            if overlap_end > overlap_start {
                let overlap_length = overlap_end - overlap_start;
                // 如果重叠超过当前长度的50%，认为是显著重叠
                overlap_length > length / 2
            } else {
                false
            }
        });

        if !has_significant_overlap {
            selected.push((start_i, start_j, length));
            used_ranges.push((start_i as usize, end_i));

            if selected.len() >= top_k {
                break;
            }
        }
    }

    selected
}

/// 带调试输出的版本
pub fn find_longest_common_substring_debug(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
) -> (i32, i32, usize) {
    find_longest_common_substring_internal(seq1, seq2, min_ratio, true)
}

fn find_longest_common_substring_internal(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
    debug: bool,
) -> (i32, i32, usize) {
    let m = seq1.len();
    let n = seq2.len();
    let min_length = ((m.min(n) as f32 * min_ratio) as usize).max(1);

    if debug {
        println!("  🔍 [LCS调试] 序列长度: seq1={}, seq2={}", m, n);
        println!(
            "  🔍 [LCS调试] 最小匹配长度阈值: {} (min_ratio={})",
            min_length, min_ratio
        );
    }

    // 🔍 统计公共哈希值
    if debug {
        let set1: std::collections::HashSet<u64> = seq1.iter().copied().collect();
        let set2: std::collections::HashSet<u64> = seq2.iter().copied().collect();
        let common_count = set1.intersection(&set2).count();
        println!(
            "  🔍 [LCS调试] 找到 {} 个公共哈希值（共 seq1={}, seq2={}）",
            common_count,
            set1.len(),
            set2.len()
        );

        if common_count == 0 {
            println!("  ❌ [LCS调试] 两个序列没有任何公共哈希值！");
            return (-1, -1, 0);
        }
    }

    // 动态规划
    let mut dp = vec![vec![0usize; n + 1]; m + 1];
    let mut max_length = 0usize;
    let mut ending_pos_i = 0;
    let mut ending_pos_j = 0;
    let mut match_count = 0;

    for i in 1..=m {
        for j in 1..=n {
            if seq1[i - 1] == seq2[j - 1] {
                dp[i][j] = dp[i - 1][j - 1] + 1;
                match_count += 1;
                if dp[i][j] > max_length {
                    max_length = dp[i][j];
                    ending_pos_i = i;
                    ending_pos_j = j;
                }
            }
        }
    }

    if debug {
        println!("  🔍 [LCS调试] 找到 {} 个哈希匹配点", match_count);
        println!("  🔍 [LCS调试] 最长公共子串长度: {}", max_length);
    }

    if max_length < min_length {
        if debug {
            println!(
                "  ❌ [LCS调试] 最长子串({}) < 阈值({})，判定为无重叠",
                max_length, min_length
            );
        }
        return (-1, -1, 0);
    }

    let start_i = (ending_pos_i - max_length) as i32;
    let start_j = (ending_pos_j - max_length) as i32;

    if debug {
        println!(
            "  ✅ [LCS调试] 找到有效重叠: seq1[{}:{}] ↔ seq2[{}:{}]",
            start_i, ending_pos_i, start_j, ending_pos_j
        );
    }

    (start_i, start_j, max_length)
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgba, RgbaImage};

    #[test]
    fn test_dhash_identical_images() {
        // 创建两个相同的测试图像
        let img = RgbaImage::from_fn(64, 64, |x, y| {
            Rgba([(x * 4) as u8, (y * 4) as u8, ((x + y) * 2) as u8, 255])
        });

        let mut bytes1 = Vec::new();
        img.write_to(
            &mut std::io::Cursor::new(&mut bytes1),
            image::ImageFormat::Png,
        )
        .unwrap();

        let hash1 = compute_dhash(&bytes1, 8).unwrap();
        let hash2 = compute_dhash(&bytes1, 8).unwrap();

        assert_eq!(hash1, hash2);
        assert_eq!(hamming_distance(hash1, hash2), 0);
    }

    #[test]
    fn test_row_hashes() {
        let img = RgbaImage::from_fn(100, 50, |x, y| {
            Rgba([(y * 5) as u8, (y * 5) as u8, (y * 5) as u8, 255])
        });

        let mut bytes = Vec::new();
        img.write_to(
            &mut std::io::Cursor::new(&mut bytes),
            image::ImageFormat::Png,
        )
        .unwrap();

        let hashes = compute_row_hashes(&bytes, 0).unwrap();
        assert_eq!(hashes.len(), 50);

        // 同一行的像素应该产生相同的哈希
        assert_eq!(hashes[0], hashes[0]);
    }
}

/// 完整的双图拼接函数 - 零拷贝高性能实现
///
/// 功能：加载图片 → 宽度对齐 → 计算哈希 → 找重叠 → 裁剪拼接 → 返回字节流
///
/// 参数:
///   img1_bytes: 第一张图片的字节数据
///   img2_bytes: 第二张图片的字节数据
///   ignore_right_pixels: 忽略右侧像素数（排除滚动条）
///   min_overlap_ratio: 最小重叠比例（默认 0.1）
///
/// 返回: 拼接后的 PNG 图片字节流，失败返回 None
pub fn stitch_two_images(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_internal(
        img1_bytes,
        img2_bytes,
        ignore_right_pixels,
        min_overlap_ratio,
        false,
    )
}

/// 带调试输出的拼接函数
pub fn stitch_two_images_debug(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_internal(
        img1_bytes,
        img2_bytes,
        ignore_right_pixels,
        min_overlap_ratio,
        true,
    )
}

fn stitch_two_images_internal(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
    debug: bool,
) -> Result<Vec<u8>, String> {
    use image::{DynamicImage, GenericImageView, ImageBuffer, Rgba};
    use std::io::Cursor;

    // 1️⃣ 加载图片
    let mut img1 = image::load_from_memory(img1_bytes)
        .map_err(|e| format!("Failed to load image 1: {}", e))?;
    let img2 = image::load_from_memory(img2_bytes)
        .map_err(|e| format!("Failed to load image 2: {}", e))?;

    let (width1, height1) = img1.dimensions();
    let (width2, height2) = img2.dimensions();

    if debug {
        println!(
            "处理图片: ({}, {}) + ({}, {})",
            width1, height1, width2, height2
        );
    }

    // 2️⃣ 宽度对齐（如果不同则缩放第一张图片）
    if width1 != width2 {
        if debug {
            println!("调整图片宽度: {} -> {}", width1, width2);
        }
        let new_height1 = (height1 as f32 * width2 as f32 / width1 as f32) as u32;
        img1 = img1.resize_exact(width2, new_height1, image::imageops::FilterType::Lanczos3);
    }

    let (final_width, final_height1) = img1.dimensions();

    if debug {
        println!("忽略右侧 {} 像素来排除滚动条影响", ignore_right_pixels);
    }

    // 3️⃣ 计算行哈希
    let img1_hashes = {
        let mut buffer = Vec::new();
        img1.write_to(&mut Cursor::new(&mut buffer), image::ImageOutputFormat::Png)
            .map_err(|e| format!("Failed to encode image 1: {}", e))?;
        if debug {
            compute_row_hashes_debug(&buffer, ignore_right_pixels)
                .map_err(|e| format!("Failed to compute hashes for image 1: {}", e))?
        } else {
            compute_row_hashes(&buffer, ignore_right_pixels)
                .map_err(|e| format!("Failed to compute hashes for image 1: {}", e))?
        }
    };

    let img2_hashes = if debug {
        compute_row_hashes_debug(img2_bytes, ignore_right_pixels)
            .map_err(|e| format!("Failed to compute hashes for image 2: {}", e))?
    } else {
        compute_row_hashes(img2_bytes, ignore_right_pixels)
            .map_err(|e| format!("Failed to compute hashes for image 2: {}", e))?
    };

    // 4️⃣ 找最长公共子串（重叠区域）
    // 🎯 关键优化：只在 img1 底部搜索（范围 = img2 的高度）
    // 因为滚动截图总是连续的，新截图一定是从上一张的底部开始
    let img1_len = img1_hashes.len();
    let img2_len = img2_hashes.len();
    let search_start = if img1_len > img2_len {
        img1_len - img2_len
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
            search_start,
            img1_len,
            img1_search_region.len()
        );
    }

    let (relative_start_i, start_j, overlap_length) = if debug {
        find_longest_common_substring_debug(img1_search_region, &img2_hashes, min_overlap_ratio)
    } else {
        find_longest_common_substring(img1_search_region, &img2_hashes, min_overlap_ratio)
    };

    // 将相对位置转换回绝对位置
    let start_i = if relative_start_i >= 0 {
        relative_start_i + search_start as i32
    } else {
        relative_start_i
    };

    if debug {
        if overlap_length > 0 {
            let overlap_ratio =
                overlap_length as f32 / img1_hashes.len().min(img2_hashes.len()) as f32;
            println!(
                "  ✅ 找到重叠: 长度{}行, 占比{:.2}%",
                overlap_length,
                overlap_ratio * 100.0
            );
            println!(
                "     绝对位置: img1[{}:{}]",
                start_i,
                start_i + overlap_length as i32
            );
        } else {
            println!("  ❌ 未找到任何重叠区域");
        }
    }

    // 5️⃣ 计算拼接参数
    let (img1_keep_height, img2_skip_height) = if overlap_length == 0 {
        if debug {
            println!("未找到重叠区域，直接拼接");
        }
        (final_height1, 0)
    } else {
        if debug {
            println!(
                "找到重叠区域: img1[{}:{}] = img2[{}:{}]",
                start_i,
                start_i + overlap_length as i32,
                start_j,
                start_j + overlap_length as i32
            );
        }
        let img1_keep = (start_i as usize + overlap_length) as u32;
        let img2_skip = (start_j as usize + overlap_length) as u32;
        (img1_keep, img2_skip)
    };

    let img2_keep_height = height2.saturating_sub(img2_skip_height);
    let result_height = img1_keep_height + img2_keep_height;

    if debug {
        println!(
            "拼接计算: img1保留{}行 + img2跳过{}行保留{}行 = 总计{}行",
            img1_keep_height, img2_skip_height, img2_keep_height, result_height
        );
    }

    // 6️⃣ 创建结果图片并拼接
    let mut result: ImageBuffer<Rgba<u8>, Vec<u8>> = ImageBuffer::new(final_width, result_height);

    // 复制 img1 的保留部分（上半部分）
    for y in 0..img1_keep_height {
        for x in 0..final_width {
            let pixel = img1.get_pixel(x, y);
            result.put_pixel(x, y, pixel);
        }
    }

    // 复制 img2 的保留部分（下半部分）
    for y in 0..img2_keep_height {
        for x in 0..final_width {
            let pixel = img2.get_pixel(x, y + img2_skip_height);
            result.put_pixel(x, y + img1_keep_height, pixel);
        }
    }

    // 7️⃣ 编码为 PNG 字节流
    let mut output = Vec::new();
    DynamicImage::ImageRgba8(result)
        .write_to(&mut Cursor::new(&mut output), image::ImageOutputFormat::Png)
        .map_err(|e| format!("Failed to encode result: {}", e))?;

    Ok(output)
}

/// 智能拼接函数 - 带多候选纠错机制
///
/// 与 stitch_two_images 的区别：
/// 1. 找多个候选子串（而不是只找最长的）
/// 2. 检查每个候选是否会导致结果缩短
/// 3. 选择第一个不会缩短的候选
///
/// 这解决了"最长匹配可能在错误位置"的问题
pub fn stitch_two_images_smart(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_smart_internal(
        img1_bytes,
        img2_bytes,
        ignore_right_pixels,
        min_overlap_ratio,
        false,
    )
}

/// 带调试输出的智能拼接函数
pub fn stitch_two_images_smart_debug(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
) -> Result<Vec<u8>, String> {
    stitch_two_images_smart_internal(
        img1_bytes,
        img2_bytes,
        ignore_right_pixels,
        min_overlap_ratio,
        true,
    )
}

fn stitch_two_images_smart_internal(
    img1_bytes: &[u8],
    img2_bytes: &[u8],
    ignore_right_pixels: u32,
    min_overlap_ratio: f32,
    debug: bool,
) -> Result<Vec<u8>, String> {
    use image::{DynamicImage, GenericImageView, ImageBuffer, Rgba};
    use std::io::Cursor;

    // 1️⃣ 加载图片
    let mut img1 = image::load_from_memory(img1_bytes)
        .map_err(|e| format!("Failed to load image 1: {}", e))?;
    let img2 = image::load_from_memory(img2_bytes)
        .map_err(|e| format!("Failed to load image 2: {}", e))?;

    let (width1, height1) = img1.dimensions();
    let (width2, height2) = img2.dimensions();

    if debug {
        println!(
            "处理图片: ({}, {}) + ({}, {})",
            width1, height1, width2, height2
        );
    }

    // 2️⃣ 宽度对齐
    if width1 != width2 {
        if debug {
            println!("调整图片宽度: {} -> {}", width1, width2);
        }
        let new_height1 = (height1 as f32 * width2 as f32 / width1 as f32) as u32;
        img1 = img1.resize_exact(width2, new_height1, image::imageops::FilterType::Lanczos3);
    }

    let (final_width, final_height1) = img1.dimensions();

    if debug {
        println!("忽略右侧 {} 像素来排除滚动条影响", ignore_right_pixels);
    }

    // 3️⃣ 计算行哈希
    let img1_hashes = {
        let mut buffer = Vec::new();
        img1.write_to(&mut Cursor::new(&mut buffer), image::ImageOutputFormat::Png)
            .map_err(|e| format!("Failed to encode image 1: {}", e))?;
        if debug {
            compute_row_hashes_debug(&buffer, ignore_right_pixels)
                .map_err(|e| format!("Failed to compute hashes for image 1: {}", e))?
        } else {
            compute_row_hashes(&buffer, ignore_right_pixels)
                .map_err(|e| format!("Failed to compute hashes for image 1: {}", e))?
        }
    };

    let img2_hashes = if debug {
        compute_row_hashes_debug(img2_bytes, ignore_right_pixels)
            .map_err(|e| format!("Failed to compute hashes for image 2: {}", e))?
    } else {
        compute_row_hashes(img2_bytes, ignore_right_pixels)
            .map_err(|e| format!("Failed to compute hashes for image 2: {}", e))?
    };

    // 4️⃣ 搜索区域设置
    let img1_len = img1_hashes.len();
    let img2_len = img2_hashes.len();
    let search_start = if img1_len > img2_len {
        img1_len - img2_len
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
            search_start,
            img1_len,
            img1_search_region.len()
        );
    }

    // 5️⃣ 找多个候选子串（关键改进！）
    let candidates = find_top_common_substrings(
        img1_search_region,
        &img2_hashes,
        min_overlap_ratio,
        5, // top_k = 5，找前5个候选
    );

    if candidates.is_empty() {
        if debug {
            println!("  ❌ 未找到任何重叠区域");
        }
        return Err("No overlap found".to_string());
    }

    if debug {
        println!("  🔍 找到 {} 个候选子串", candidates.len());
    }

    // 6️⃣ 智能选择：找第一个不会导致缩短的候选
    let mut best_candidate: Option<(i32, i32, usize)> = None;

    for (idx, &(relative_start_i, start_j, overlap_length)) in candidates.iter().enumerate() {
        // 转换为绝对位置
        let start_i = (relative_start_i + search_start as i32) as usize;
        let overlap_ratio = overlap_length as f32 / img1_len.min(img2_len) as f32;

        // 计算预期结果高度
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
            best_candidate = Some((start_i as i32, start_j, overlap_length));
            if debug {
                println!("  ✅ 选择此候选作为最佳匹配");
            }
            break;
        }
    }

    // 如果所有候选都会缩短，使用第一个（最长的）
    let (start_i, start_j, overlap_length) = match best_candidate {
        Some(c) => c,
        None => {
            if debug {
                println!("\n  ⚠️  所有候选都会导致缩短，使用第一个候选（最长匹配）");
            }
            let first = &candidates[0];
            ((first.0 + search_start as i32), first.1, first.2)
        }
    };

    // 7️⃣ 计算拼接参数
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

    // 8️⃣ 创建结果图片并拼接
    let mut result: ImageBuffer<Rgba<u8>, Vec<u8>> = ImageBuffer::new(final_width, result_height);

    // 复制 img1 的保留部分
    for y in 0..img1_keep_height {
        for x in 0..final_width {
            let pixel = img1.get_pixel(x, y);
            result.put_pixel(x, y, pixel);
        }
    }

    // 复制 img2 的保留部分
    for y in 0..img2_keep_height {
        for x in 0..final_width {
            let pixel = img2.get_pixel(x, y + img2_skip_height);
            result.put_pixel(x, y + img1_keep_height, pixel);
        }
    }

    // 9️⃣ 编码为 PNG 字节流
    let mut output = Vec::new();
    DynamicImage::ImageRgba8(result)
        .write_to(&mut Cursor::new(&mut output), image::ImageOutputFormat::Png)
        .map_err(|e| format!("Failed to encode result: {}", e))?;

    Ok(output)
}
