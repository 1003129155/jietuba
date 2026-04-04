/// LCS（最长公共子串）模块 - 用于找到重叠区域
///
/// 提供单匹配和多候选匹配两种接口

use std::collections::{HashMap, HashSet};

/// 找到两个哈希序列的最长公共子串
pub fn find_longest_common_substring(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
) -> (i32, i32, usize) {
    find_longest_common_substring_internal(seq1, seq2, min_ratio, false)
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

        let set1: HashSet<u64> = seq1.iter().copied().collect();
        let set2: HashSet<u64> = seq2.iter().copied().collect();
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

    // 动态规划（滚动数组，O(n) 内存）
    let mut prev = vec![0usize; n + 1];
    let mut curr = vec![0usize; n + 1];
    let mut max_length = 0usize;
    let mut ending_pos_i = 0;
    let mut ending_pos_j = 0;
    let mut match_count = 0u64;

    for i in 1..=m {
        for val in curr.iter_mut() {
            *val = 0;
        }
        for j in 1..=n {
            if seq1[i - 1] == seq2[j - 1] {
                curr[j] = prev[j - 1] + 1;
                match_count += 1;
                if curr[j] > max_length {
                    max_length = curr[j];
                    ending_pos_i = i;
                    ending_pos_j = j;
                }
            }
        }
        std::mem::swap(&mut prev, &mut curr);
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

/// 找到多个公共子串候选（用于智能拼接纠错）
///
/// 返回前 top_k 个最长的不重叠公共子串
pub fn find_top_common_substrings(
    seq1: &[u64],
    seq2: &[u64],
    min_ratio: f32,
    top_k: usize,
) -> Vec<(i32, i32, usize)> {
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

    substrings.sort_by(|a, b| b.2.cmp(&a.2));
    substrings.dedup();

    // 选择不重叠的前 top_k 个
    let mut selected = Vec::new();
    let mut used_ranges: Vec<(usize, usize)> = Vec::new();

    for (start_i, start_j, length) in substrings {
        let end_i = start_i as usize + length;

        let has_significant_overlap = used_ranges.iter().any(|(used_start, used_end)| {
            let overlap_start = (*used_start).max(start_i as usize);
            let overlap_end = (*used_end).min(end_i);

            if overlap_end > overlap_start {
                let overlap_length = overlap_end - overlap_start;
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
