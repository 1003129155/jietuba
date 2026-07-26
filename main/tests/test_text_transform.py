# -*- coding: utf-8 -*-

import pytest

from clipboard.core.text_transform import to_sql_in_clause


def test_to_sql_in_clause_splits_common_delimiters():
    text = "hello world,foo;\nbar\tbaz"

    assert to_sql_in_clause(text) == (
        "'hello', 'world', 'foo', 'bar', 'baz'"
    )


def test_to_sql_in_clause_returns_empty_text_for_blank_input():
    assert to_sql_in_clause(" \r\n\t,; ") == ""


def test_to_sql_in_clause_escapes_single_quotes():
    assert to_sql_in_clause("O'Reilly") == "'O''Reilly'"


def test_to_sql_in_clause_wraps_only_between_values():
    result = to_sql_in_clause("a bbbbbbbb c", max_line_length=19)

    assert result == "'a', 'bbbbbbbb',\n'c'"
    assert all(len(line) <= 19 for line in result.splitlines())


def test_to_sql_in_clause_allows_a_single_long_value_to_exceed_limit():
    value = "x" * 30

    result = to_sql_in_clause(value, max_line_length=20)

    assert result == f"'{value}'"
    assert "\n" not in result


def test_to_sql_in_clause_rejects_non_positive_line_length():
    with pytest.raises(ValueError, match="max_line_length 必须大于 0"):
        to_sql_in_clause("a", max_line_length=0)
