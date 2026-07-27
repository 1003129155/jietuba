"""
翻译接口对比测试 —— 用当前配置中的 DeepL 和 Amazon Translate 分别翻译一组测试句，
输出结果供人工判断正确率。
"""

import sys
import os

# 确保 main/ 在 sys.path
main_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if main_dir not in sys.path:
    sys.path.insert(0, main_dir)

from translation.service import create_default_translation_service
from translation.models import TranslationRequest

# ── 测试句子（换一批词） ──────────────────────────────────
TEST_CASES: list[dict] = [
    dict(
        label="英→中 (日常问候)",
        text="Could you please pass me the salt? Thanks a lot!",
        source_lang="en",
        target_lang="zh-Hans",
    ),
    dict(
        label="英→中 (金融术语)",
        text="The bull market has driven stock prices to an all-time high, boosting investor confidence.",
        source_lang="en",
        target_lang="zh-Hans",
    ),
    dict(
        label="英→中 (长句/逻辑)",
        text="Although the initial investment was substantial, the long-term return on investment justifies the expenditure when factoring in inflation and market growth.",
        source_lang="en",
        target_lang="zh-Hans",
    ),
    dict(
        label="中→英 (感慨)",
        text="时间过得真快，一转眼我们都已经认识十年了。",
        source_lang="zh-Hans",
        target_lang="en",
    ),
    dict(
        label="中→英 (AI领域)",
        text="大语言模型通过海量文本训练，学会了理解上下文、生成连贯文本，甚至能够编写代码和创作诗歌。",
        source_lang="zh-Hans",
        target_lang="en",
    ),
    dict(
        label="中→英 (俗语)",
        text="路遥知马力，日久见人心。相处久了才能真正了解一个人。",
        source_lang="zh-Hans",
        target_lang="en",
    ),
    dict(
        label="英→日 (邀约)",
        text="Would you like to grab a cup of coffee this weekend? I know a great place.",
        source_lang="en",
        target_lang="ja",
    ),
    dict(
        label="英→日 (法律)",
        text="The contract shall be governed by and construed in accordance with the laws of Japan.",
        source_lang="en",
        target_lang="ja",
    ),
    dict(
        label="日→英",
        text="この度はお世話になりました。また機会がございましたら、ぜひよろしくお願いいたします。",
        source_lang="ja",
        target_lang="en",
    ),
    dict(
        label="日→中 (道歉)",
        text="先日は大変失礼なことを申してしまい、誠に申し訳ございませんでした。深く反省しております。",
        source_lang="ja",
        target_lang="zh-Hans",
    ),
    dict(
        label="中→英 (网络用语)",
        text="这个视频真的太搞笑了，我直接笑到肚子疼，强烈推荐大家去看看！",
        source_lang="zh-Hans",
        target_lang="en",
    ),
    dict(
        label="英→中 (双关/歧义)",
        text="The fisherman made a great catch, but his wife was upset he was late for dinner — what a catch-22!",
        source_lang="en",
        target_lang="zh-Hans",
    ),
    dict(
        label="中→日 (询问)",
        text="请问这个文件要怎么修改？我试了好几次都不成功。",
        source_lang="zh-Hans",
        target_lang="ja",
    ),
    dict(
        label="日→中 (新闻)",
        text="本日午後、都心で大規模な停電が発生し、約5万世帯に影響が出ています。復旧のめどは立っていません。",
        source_lang="ja",
        target_lang="zh-Hans",
    ),
    dict(
        label="英→日 (情感)",
        text="This song always brings tears to my eyes whenever I hear it.",
        source_lang="en",
        target_lang="ja",
    ),
    dict(
        label="中→英 (古风)",
        text="此情可待成追忆，只是当时已惘然。",
        source_lang="zh-Hans",
        target_lang="en",
    ),
    dict(
        label="日→英 (IT)",
        text="このアプリケーションはクラウド上で動作しており、ユーザーデータはすべて暗号化されています。",
        source_lang="ja",
        target_lang="en",
    ),
]


def main():
    service = create_default_translation_service()

    providers = ["deepl", "amazon", "google"]
    results = {p: [] for p in providers}

    for case in TEST_CASES:
        label = case["label"]
        text = case["text"]
        source = case.get("source_lang")
        target = case["target_lang"]

        print(f"\n{'='*70}")
        print(f"📝 {label}")
        print(f"  原文: {text[:80]}{'…' if len(text)>80 else ''}")
        print(f"  {source or 'auto'} → {target}")

        for pid in providers:
            try:
                req = TranslationRequest(
                    text=text,
                    target_lang=target,
                    source_lang=source,
                    preserve_formatting=True,
                    timeout=15,
                )
                result = service.translate(req, provider_id=pid)
                results[pid].append((label, result))
                if result.success:
                    print(f"\n  ✅ [{pid.upper()}] {result.translated_text[:100]}")
                else:
                    print(f"\n  ❌ [{pid.upper()}] 失败: {result.error_message}")
            except Exception as e:
                print(f"\n  ❌ [{pid.upper()}] 异常: {e}")
                results[pid].append((label, None))

    # ── 汇总比较 ──
    print("\n\n")
    print("=" * 70)
    print("📊 汇总比较")
    print("=" * 70)
    for i, case in enumerate(TEST_CASES):
        label = case["label"]
        deepl_result = results["deepl"][i][1]
        amazon_result = results["amazon"][i][1]
        google_result = results["google"][i][1]

        print(f"\n--- {label} ---")
        print(f"  🔤 原文: {case['text'][:80]}")
        if deepl_result and deepl_result.success:
            print(f"  🔵 DeepL: {deepl_result.translated_text[:120]}")
        else:
            print(f"  🔵 DeepL: ❌ {deepl_result.error_message if deepl_result else 'N/A'}")
        if amazon_result and amazon_result.success:
            print(f"  🟢 Amazon: {amazon_result.translated_text[:120]}")
        else:
            print(f"  🟢 Amazon: ❌ {amazon_result.error_message if amazon_result else 'N/A'}")
        if google_result and google_result.success:
            print(f"  🔴 Google: {google_result.translated_text[:120]}")
        else:
            print(f"  🔴 Google: ❌ {google_result.error_message if google_result else 'N/A'}")

    # 简单统计
    deepl_ok = sum(1 for r in results["deepl"] if r[1] and r[1].success)
    amazon_ok = sum(1 for r in results["amazon"] if r[1] and r[1].success)
    google_ok = sum(1 for r in results["google"] if r[1] and r[1].success)
    print(f"\n{'='*70}")
    print(f"✅ DeepL 成功:  {deepl_ok}/{len(TEST_CASES)}")
    print(f"✅ Amazon 成功: {amazon_ok}/{len(TEST_CASES)}")
    print(f"✅ Google 成功: {google_ok}/{len(TEST_CASES)}")


if __name__ == "__main__":
    main()
