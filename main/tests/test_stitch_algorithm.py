# -*- coding: utf-8 -*-
"""
长截图拼接算法单元测试

测试 find_top_common_substrings / find_longest_common_substring 等纯算法。
"""
import pytest
from stitch.jietuba_long_stitch import (
    find_top_common_substrings,
    find_longest_common_substring,
    reset_performance_stats,
    _performance_stats,
)


@pytest.fixture(autouse=True)
def reset_stats():
    """每个测试前重置性能统计"""
    reset_performance_stats()
    yield


# ==================== find_top_common_substrings 测试 ====================

class TestFindTopCommonSubstrings:
    """多公共子串搜索测试"""

    def test_identical_sequences(self):
        """完全相同的序列"""
        seq = [1, 2, 3, 4, 5]
        result = find_top_common_substrings(seq, seq, min_ratio=0.1, top_k=5)
        assert len(result) >= 1
        # 最长匹配应该等于序列长度
        assert result[0][2] == 5

    def test_no_common(self):
        """完全不同的序列"""
        seq1 = [1, 2, 3]
        seq2 = [4, 5, 6]
        result = find_top_common_substrings(seq1, seq2, min_ratio=0.1, top_k=5)
        assert result == []

    def test_partial_overlap(self):
        """部分重叠"""
        seq1 = [1, 2, 3, 4, 5]
        seq2 = [3, 4, 5, 6, 7]
        result = find_top_common_substrings(seq1, seq2, min_ratio=0.1, top_k=5)
        assert len(result) >= 1
        # 最长公共子串应该是 [3, 4, 5]，长度为 3
        best = result[0]
        assert best[2] == 3

    def test_min_ratio_filter(self):
        """最小比例过滤"""
        seq1 = list(range(100))
        seq2 = list(range(50, 150))
        # min_ratio=0.5 意味着最小长度为 50
        result = find_top_common_substrings(seq1, seq2, min_ratio=0.5, top_k=5)
        assert len(result) >= 1
        assert result[0][2] == 50

    def test_top_k_limit(self):
        """top_k 限制返回数量"""
        seq1 = [1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3]
        seq2 = [1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3]
        result = find_top_common_substrings(seq1, seq2, min_ratio=0.01, top_k=2)
        assert len(result) <= 2

    def test_positions_correct(self):
        """返回的位置索引应正确"""
        seq1 = [0, 0, 1, 2, 3, 0, 0]
        seq2 = [9, 9, 1, 2, 3, 9, 9]
        result = find_top_common_substrings(seq1, seq2, min_ratio=0.1, top_k=5)
        assert len(result) >= 1
        start_i, start_j, length = result[0]
        # 验证匹配的实际内容一致
        assert seq1[start_i:start_i + length] == seq2[start_j:start_j + length]

    def test_empty_sequences(self):
        """空序列"""
        result = find_top_common_substrings([], [], min_ratio=0.1, top_k=5)
        assert result == []


# ==================== find_longest_common_substring 测试 ====================

class TestFindLongestCommonSubstring:
    """最长公共子串测试"""

    def test_basic_match(self):
        """基本匹配"""
        seq1 = [1, 2, 3, 4, 5]
        seq2 = [3, 4, 5, 6, 7]
        start_i, start_j, length = find_longest_common_substring(seq1, seq2, min_ratio=0.1)
        assert length == 3
        assert seq1[start_i:start_i + length] == [3, 4, 5]

    def test_no_match(self):
        """无匹配"""
        seq1 = [1, 2, 3]
        seq2 = [4, 5, 6]
        _, _, length = find_longest_common_substring(seq1, seq2, min_ratio=0.1)
        assert length == 0

    def test_full_overlap(self):
        """一个序列完全包含在另一个中"""
        seq1 = [0, 1, 2, 3, 4, 5, 0]
        seq2 = [1, 2, 3, 4, 5]
        start_i, start_j, length = find_longest_common_substring(seq1, seq2, min_ratio=0.1)
        assert length == 5

    def test_scrolling_capture_simulation(self):
        """模拟滚动截图场景：两张图片底部-顶部重叠"""
        # img1: 100行，底部50行与 img2 顶部50行重叠
        shared = list(range(1000, 1050))
        img1 = list(range(0, 50)) + shared
        img2 = shared + list(range(2000, 2050))
        
        start_i, start_j, length = find_longest_common_substring(img1, img2, min_ratio=0.1)
        assert length == 50
        assert start_i == 50  # 在 img1 中从第50行开始
        assert start_j == 0   # 在 img2 中从第0行开始


# ==================== 性能统计测试 ====================

class TestPerformanceStats:
    """性能统计测试"""

    def test_reset_stats(self):
        """重置后统计应为零"""
        reset_performance_stats()
        assert _performance_stats['hash_time'] == 0.0
        assert _performance_stats['lcs_time'] == 0.0
        assert _performance_stats['hash_count'] == 0
        assert _performance_stats['lcs_count'] == 0

    def test_lcs_updates_stats(self):
        """执行 LCS 后统计应更新"""
        seq1 = [1, 2, 3, 4, 5]
        seq2 = [3, 4, 5, 6, 7]
        find_longest_common_substring(seq1, seq2, min_ratio=0.1)
        assert _performance_stats['lcs_count'] >= 1
 