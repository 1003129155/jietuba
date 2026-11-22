"""Where pages come from: Wikipedia text for the synthetic scenes, and real web pages."""
import json
import re
import time
import urllib.parse

import requests

SESSION = requests.Session()
SESSION.headers["User-Agent"] = "jietuba-paragraph-model/1.0 (training data for a screenshot tool)"
WIKI_LANGS = ("en", "zh", "ja", "ko")


def _api(lang, params):
    for attempt in range(5):
        try:
            return SESSION.get(f"https://{lang}.wikipedia.org/w/api.php", params=dict(params, format="json"), timeout=20).json()
        except Exception:
            time.sleep(2 + attempt * 2)
    return {}


def fetch_corpus(path, rounds=6):
    """Intro paragraphs of random articles per language, for the text of synthetic scenes."""
    try:
        from zhconv import convert
    except ImportError:
        convert = None
    corpus = {}
    for lang in WIKI_LANGS:
        paragraphs, titles = [], []
        for _ in range(rounds):
            data = _api(lang, {"action": "query", "generator": "random", "grnnamespace": 0, "grnlimit": 20, "prop": "extracts", "exintro": 1,
                               "explaintext": 1, "exlimit": "max", "redirects": 1})
            for page in data.get("query", {}).get("pages", {}).values():
                titles.append(page["title"])
                for text in (page.get("extract") or "").split("\n"):
                    text = re.sub(r"\s+", " ", text).strip()
                    if len(text) >= (90 if lang == "en" else 45):
                        paragraphs.append(text)
            time.sleep(1.0)
        if lang == "zh" and convert:
            paragraphs = [convert(p, "zh-cn") for p in paragraphs]
            titles = [convert(t, "zh-cn") for t in titles]
        corpus[lang] = {"paragraphs": paragraphs, "titles": titles}
    path.write_text(json.dumps(corpus, ensure_ascii=False), encoding="utf-8")
    return corpus


def random_wiki_pages(lang, count, exclude=()):
    """The longest of a draw of random articles: stubs have too little text to show a layout."""
    urls = []
    for _ in range(8):
        data = _api(lang, {"action": "query", "list": "random", "rnnamespace": 0, "rnlimit": 40})
        urls += [f"https://{lang}.wikipedia.org/wiki/" + urllib.parse.quote(p["title"].replace(" ", "_")) for p in data.get("query", {}).get("random", [])]
        if len(urls) >= count * 4:
            break
    urls = [u for u in dict.fromkeys(urls) if u not in exclude]
    sizes = {}
    titles = [urllib.parse.unquote(u.rsplit("/wiki/", 1)[1]).replace("_", " ") for u in urls]
    for k in range(0, len(titles), 40):
        data = _api(lang, {"action": "query", "prop": "info", "titles": "|".join(titles[k:k + 40])})
        for page in data.get("query", {}).get("pages", {}).values():
            sizes[page["title"]] = page.get("length", 0)
    return sorted(urls, key=lambda u: -sizes.get(urllib.parse.unquote(u.rsplit("/wiki/", 1)[1]).replace("_", " "), 0))[:count]
