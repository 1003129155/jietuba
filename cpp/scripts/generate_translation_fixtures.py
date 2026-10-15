"""Developer-only oracle for translation/ providers and helpers.

Run with the reference interpreter:
    ..\\venv311\\Scripts\\python.exe scripts\\generate_translation_fixtures.py
"""
from __future__ import annotations

import datetime as dt
import html
import io
import json
import os
import random
import sys
import tempfile
import urllib.error
import urllib.parse
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True  # the reference tree tracks its __pycache__
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'main'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from translation.models import TranslationRequest  # noqa: E402
from translation.providers.amazon import AmazonTranslateProvider  # noqa: E402
from translation.providers.baidu import BaiduTranslateProvider  # noqa: E402
from translation.providers.custom_llm import CUSTOM_LLM_PROVIDERS  # noqa: E402
from translation.providers.deepseek import DeepSeekProvider  # noqa: E402
from translation.providers.openai_compatible import OpenAICompatibleProvider  # noqa: E402
from translation.providers.tencent import TencentTranslateProvider  # noqa: E402
from translation.providers.volcengine import VolcengineTranslateProvider  # noqa: E402
from translation.provider import ModelField, ToggleField  # noqa: E402
from settings.tool_settings import ToolSettingsManager  # noqa: E402
from PySide6.QtCore import QSettings  # noqa: E402

random.seed(20261013)

pieces = ['&amp;', '&amp', '&lt;', '&gt', '&quot;', '&#39;', '&#x27;', '&#X4e2d;', '&#20013;', '&#0;', '&#128;', '&#x80;',
          '&#xD800;', '&#1114112;', '&#99999999999999;', '&#11;', '&notit;', '&notin;', '&ampx', '&unknown;', '&;', '&#;',
          '&#x;', '& ', '&', '&&amp;', '&nbsp', '&Aacute', '&aacutefoo', '&copy;', '&CounterClockwiseContourIntegral;',
          'text', ' ', '中文', '\n', ';', '#']
unescape = []
for case in ['', 'plain', 'a & b', 'Tom &amp; Jerry', 'x&lt;y&gt;z', '&' + 'a' * 40 + ';']:
    unescape.append({'input': case, 'expected': html.unescape(case)})
for _ in range(300):
    text = ''.join(random.choice(pieces) for _ in range(random.randint(1, 6)))
    unescape.append({'input': text, 'expected': html.unescape(text)})

extract_inputs = [
    '{"translation": "你好", "detected_source_lang": "en"}',
    '```json\n{"translation": "Hi", "detected_source_lang": "zh-Hans"}\n```',
    '<think>\nreasoning\n</think>\n{"translation": " spaced ", "detected_source_lang": " ja "}',
    '  <think>a</think><think>b</think>{"translation": "x"}',
    'Plain text reply',
    '```\nonly fence\n```',
    '```\n```',
    '{"translation": ""}',
    '{"translation": 42, "detected_source_lang": null}',
    '["a", "b"]',
    '"just a string"',
    '{"other": 1}',
    '　{"translation": "全角空格"}　',
]
extract = [{'input': text, 'expected': list(OpenAICompatibleProvider._extract(text))} for text in extract_inputs]

base_urls = ['https://api.openai.com/v1', 'https://api.openai.com/v1/', 'https://x.test/v1/chat/completions',
             'https://x.test/v1/Chat/Completions/', ' http://localhost:11434/v1// ', 'https://x.test/chat/completions//', '']
normalize = [{'input': url, 'expected': OpenAICompatibleProvider._normalize_base_url(url)} for url in base_urls]

responses = [
    {'choices': [{'message': {'content': '{"translation": "ok", "detected_source_lang": "en"}'}}]},
    {'choices': [{'message': {'content': ''}, 'finish_reason': 'content_filter'}]},
    {'choices': [{'message': {'content': '', 'reasoning_content': 'thinking'}, 'finish_reason': 'length'}]},
    {'choices': [{'message': {'content': None}}]},
    {'choices': []},
    {'error': {'message': 'Insufficient Balance', 'type': 'insufficient_quota'}},
    {'error': {'message': 'bad key', 'type': 'authentication_error'}},
    {'error': 'string error'},
    [1, 2],
]
parse = []
for provider_class, config in [(DeepSeekProvider, {'api_key': 'k'}), (CUSTOM_LLM_PROVIDERS[0], {'base_url': 'http://x', 'model': 'm'})]:
    provider = provider_class(config)
    for raw in responses:
        result = provider._parse_response(raw)
        parse.append({'provider': provider.provider_id, 'raw': raw, 'success': result.success,
                      'translated_text': result.translated_text, 'detected_source_lang': result.detected_source_lang,
                      'error_code': result.error_code.value if result.error_code else None,
                      'error_message': result.error_message})

custom_configs = [
    {},
    {'base_url': 'http://localhost:11434/v1', 'model': 'qwen'},
    {'base_url': 'http://x', 'model': 'm', 'name': ' Mine ', 'temperature': '0.7', 'timeout': '120'},
    {'base_url': 'http://x', 'model': 'm', 'temperature': 'abc', 'timeout': 'x', 'extra_body': '[1]'},
    {'base_url': 'http://x', 'model': 'm', 'timeout': '1', 'extra_body': '{"reasoning_effort": "none"}'},
    {'base_url': 'http://x', 'model': 'm', 'timeout': '9999.9', 'json_mode': False},
    {'base_url': 'http://x', 'model': 'm', 'timeout': 'nan', 'extra_body': '{bad'},
]
custom = []
for slot, provider_class in enumerate(CUSTOM_LLM_PROVIDERS, start=1):
    for config in custom_configs:
        provider = provider_class(config)
        custom.append({'slot': slot, 'config': config, 'provider_id': provider.provider_id,
                       'is_configured': provider.is_configured(), 'config_error': provider._config_error,
                       'min_timeout': provider.MIN_TIMEOUT, 'temperature': provider._temperature,
                       'json_mode': provider._json_mode, 'extra_body': provider._extra_body,
                       'display_label': provider.display_label('Custom LLM'),
                       'body': provider._request_body(TranslationRequest('hello', 'zh-Hans', 'en'), provider._json_mode)})

amazon = []
for config, when, payload in [
    ({'region': 'us-west-2', 'access_key_id': 'AKIDEXAMPLE', 'secret_access_key': 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY'},
     dt.datetime(2026, 10, 13, 8, 30, 15, tzinfo=dt.timezone.utc), '{"Text":"hello"}'),
    ({'region': 'cn-north-1', 'access_key_id': 'AK', 'secret_access_key': 'SK', 'session_token': ' token  with  spaces '},
     dt.datetime(2025, 1, 2, 3, 4, 5, tzinfo=dt.timezone.utc), '{"Text":"中文"}'),
]:
    provider = AmazonTranslateProvider(config)
    url, headers = provider._signed_request(payload.encode('utf-8'), now=when)
    amazon.append({'config': config, 'now': when.strftime('%Y-%m-%dT%H:%M:%SZ'), 'payload': payload, 'url': url,
                   'headers': headers})

baidu = []
for appid, key, text, salt in [('2015063000000001', '12345678', 'apple', 1435660288), ('appid', 'secret', '你好，世界', 123456)]:
    provider = BaiduTranslateProvider({'appid': appid, 'secret_key': key})
    baidu.append({'appid': appid, 'secret_key': key, 'text': text, 'salt': salt, 'expected': provider._sign(text, salt)})

urlencode_cases = [[['key', 'a b&c=d/é~_.-*']], [['api-version', '3.0'], ['to', 'zh-Hans']], [['text', '中文\n+%']]]
urlencode = [{'query': query, 'expected': urllib.parse.urlencode([tuple(pair) for pair in query])} for query in urlencode_cases]

# ---- Volcengine and Tencent: metadata, signatures, request bodies and every response path ----
MT = {'volcengine': (VolcengineTranslateProvider, {'access_key_id': 'AKIDvolc', 'secret_access_key': 'volc-secret'}),
      'tencent': (TencentTranslateProvider, {'secret_id': 'AKIDtencent', 'secret_key': 'tencent-secret'})}


def field_info(f):
    kind = 'toggle' if isinstance(f, ToggleField) else 'model' if isinstance(f, ModelField) else 'text'
    return {'kind': kind, 'config_key': f.config_key, 'label': f.label, 'placeholder': getattr(f, 'placeholder', ''),
            'secret': getattr(f, 'secret', False), 'description': getattr(f, 'description', '')}


mt_metadata = []
for provider_class, _ in MT.values():
    meta = provider_class.metadata()
    mt_metadata.append({'provider_id': meta.provider_id, 'display_name': meta.display_name,
                        'credentials': [field_info(f) for f in meta.credentials],
                        'options': [field_info(f) for f in meta.options],
                        'supported_request_options': sorted(meta.supported_request_options), 'notice': meta.notice,
                        'help_label': meta.help_label, 'help_url': meta.help_url,
                        'min_timeout': provider_class.MIN_TIMEOUT, 'quota_limit_chars': provider_class.QUOTA_LIMIT_CHARS})

volcengine_signature = []
for config, x_date, payload in [
    (MT['volcengine'][1], '20261014T083015Z', '{"SourceLanguage":"","TargetLanguage":"zh","TextList":["Hello"]}'),
    ({'access_key_id': ' AK ', 'secret_access_key': ' 密钥SK '}, '20250102T030405Z',
     '{"SourceLanguage":"en","TargetLanguage":"ja","TextList":["中文\\n第二行"]}'),
]:
    headers = VolcengineTranslateProvider(config)._signed_headers(payload.encode('utf-8'), x_date)
    volcengine_signature.append({'config': config, 'x_date': x_date, 'payload': payload, 'headers': [[k, v] for k, v in headers.items()]})

# 1735775999 is 23:59:59 UTC, already the next day in UTC+8.
tencent_signature = []
for config, timestamp, payload in [
    (MT['tencent'][1], 1760430615, '{"SourceText":"Hello","Source":"auto","Target":"zh","ProjectId":0}'),
    ({'secret_id': ' id ', 'secret_key': ' 密钥 '}, 1735775999, '{"SourceText":"中文","Source":"zh","Target":"en","ProjectId":0}'),
]:
    headers = TencentTranslateProvider(config)._signed_headers(payload.encode('utf-8'), str(timestamp))
    tencent_signature.append({'config': config, 'timestamp': timestamp, 'payload': payload, 'headers': [[k, v] for k, v in headers.items()]})


class _Response:
    def __init__(self, data):
        self._data = data

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def read(self):
        return self._data


def run_translate(provider, request, urlopen):
    # log_error is silenced so the oracle never writes to the user's logs.
    module = sys.modules[type(provider).__module__]
    with mock.patch.object(module.urllib.request, 'urlopen', urlopen), mock.patch.object(module, 'log_error', lambda *a, **k: None):
        return provider.translate(request)


def result_info(result):
    return {'success': result.success, 'translated_text': result.translated_text,
            'detected_source_lang': result.detected_source_lang,
            'error_code': result.error_code.value if result.error_code else None, 'error_message': result.error_message}


ok_bodies = {'volcengine': {'TranslationList': [{'Translation': 'ok'}]}, 'tencent': {'Response': {'TargetText': 'ok'}}}
mt_requests = []
for provider_id, (provider_class, config) in MT.items():
    for text, target, source in [('Hello', 'zh-Hans', 'en'), ('你好', 'zh-Hans', None), ('你好\n世界', 'EN', 'auto'),
                                 ('x', 'zh-Hant', 'zh-Hant'), ('x', 'pt-BR', 'en-GB'), ('x', 'ja', 'ko'), ('"q" \\ /', 'fr', 'de')]:
        captured = {}

        def capture(request, timeout, captured=captured, provider_id=provider_id):
            captured.update(url=request.full_url, body=json.loads(request.data.decode('utf-8')), timeout=timeout)
            return _Response(json.dumps(ok_bodies[provider_id]).encode('utf-8'))

        request = TranslationRequest(text, target, source, timeout=7)
        result = run_translate(provider_class(config), request, capture)
        mt_requests.append({'provider': provider_id, 'text': text, 'target': target, 'source': source, 'url': captured['url'],
                            'body': captured['body'], 'timeout': captured['timeout'], 'result': result_info(result)})


def answer(body):
    data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode('utf-8')
    return {'kind': 'ok', 'body': data}


def http_error(status, body=b'', reason='reason'):
    data = body if isinstance(body, bytes) else json.dumps(body).encode('utf-8')
    return {'kind': 'http_error', 'status': status, 'reason': reason, 'body': data}


common_scenarios = [answer(b'not json'), answer(b''), answer(b'\xff\xfe'), answer([]), answer({}),
                    http_error(403), http_error(401), http_error(503), http_error(400, b'not json'), http_error(404, reason=''),
                    {'kind': 'timeout'}, {'kind': 'network_error', 'reason': 'offline'}]
scenarios = {
    'volcengine': common_scenarios + [
        answer({'ResponseMetadata': {'RequestId': 'r1'}, 'TranslationList': [{'Translation': '你好', 'DetectedSourceLanguage': 'en'}]}),
        answer({'TranslationList': [{'Translation': 'Hi', 'DetectedSourceLanguage': 'zh'}]}),
        answer({'TranslationList': [{'Translation': 'Hello'}, {'Translation': 'World'}]}),
        answer({'TranslationList': [{'Translation': 'A'}, 'junk', {'Translation': 'B'}]}),
        answer({'TranslationList': ['junk', {'Translation': 'B', 'DetectedSourceLanguage': 'ja'}]}),
        answer({'TranslationList': [{'Translation': 'A'}, {'Translation': ''}]}),
        answer({'TranslationList': [{'Translation': 42, 'DetectedSourceLanguage': 'zh-Hant'}]}),
        answer({'TranslationList': [{'Translation': ''}]}),
        answer({'TranslationList': []}),
        answer({'TranslationList': {'Translation': 'x'}}),
        answer({'ResponseMetadata': 'x', 'TranslationList': [{'Translation': 'ok'}]}),
        answer({'ResponseMetadata': {'Error': {}}, 'TranslationList': [{'Translation': 'ok'}]}),
        answer({'ResponseMetadata': {'Error': {'Code': '-415', 'Message': 'unsupported pair'}}}),
        answer({'ResponseMetadata': {'Error': {'Code': -500, 'Message': 'internal error'}}}),
        answer({'ResponseMetadata': {'Error': {'Code': '-429'}}}),
        answer({'ResponseMetadata': {'Error': {'Code': '100010', 'Message': ''}}}),
        http_error(429, {'ResponseMetadata': {'Error': {'Code': '-429', 'Message': 'slow down'}}}),
        http_error(500, {'ResponseMetadata': {'Error': {}}}),
    ],
    'tencent': common_scenarios + [
        answer({'Response': {'RequestId': 'r1', 'Source': 'en', 'Target': 'zh', 'TargetText': '你好'}}),
        answer({'Response': {'TargetText': '你好', 'Source': 'zh-TW'}}),
        answer({'Response': {'TargetText': 'Hi', 'Source': 'zh'}}),
        answer({'Response': {'TargetText': 'Hi'}}),
        answer({'Response': {'TargetText': 0}}),
        answer({'Response': {}}),
        answer({'Response': 'x'}),
        answer({'Response': {'Error': 'x', 'TargetText': 'still'}}),
        answer({'Response': {'Error': {'Code': 'FailedOperation.NoFreeAmount', 'Message': 'out of free quota'}}}),
        answer({'Response': {'Error': {'Code': 'AuthFailure.SecretIdNotFound'}}}),
        answer({'Response': {'Error': {}}}),
        http_error(400, {'Response': {'Error': {'Code': 'AuthFailure.SignatureFailure', 'Message': 'bad sig'}}}),
        http_error(400, {'Response': {'Error': 'x'}}),
    ],
}
mt_responses = []
for provider_id, (provider_class, config) in MT.items():
    for scenario in scenarios[provider_id]:
        def urlopen(request, timeout, scenario=scenario):
            if scenario['kind'] == 'timeout':
                raise TimeoutError()
            if scenario['kind'] == 'network_error':
                raise urllib.error.URLError(scenario['reason'])
            if scenario['kind'] == 'http_error':
                raise urllib.error.HTTPError('https://example.invalid', scenario['status'], scenario['reason'], {},
                                             io.BytesIO(scenario['body']))
            return _Response(scenario['body'])

        result = run_translate(provider_class(config), TranslationRequest('Hello', 'ja', timeout=7), urlopen)
        stored = dict(scenario)
        if 'body' in stored:
            stored['body_hex'] = stored.pop('body').hex()
        mt_responses.append({'provider': provider_id, 'scenario': stored, 'result': result_info(result)})


def no_network(request, timeout):
    raise AssertionError('no request expected')


# Every case returns before any request.
mt_prechecks = []
for provider_id, (provider_class, config) in MT.items():
    cases = [(config, '  ', 'ja'), (config, '\n\t', 'ja'), ({}, 'Hello', 'ja'), ({key: ' ' for key in config}, 'Hello', 'ja')]
    if provider_id == 'tencent':
        cases += [(config, 'Hello', target) for target in ('nl', 'pl', 'uk')]
    for case_config, text, target in cases:
        result = run_translate(provider_class(case_config), TranslationRequest(text, target), no_network)
        mt_prechecks.append({'provider': provider_id, 'config': case_config, 'text': text, 'target': target,
                             'is_configured': provider_class(case_config).is_configured(), 'result': result_info(result)})

mt_error_codes = []
for code in ['-400', '-415', '-429', '-500', '100010', '', '415', 'x-415']:
    mt_error_codes.append({'provider': 'volcengine', 'code': code, 'expected': VolcengineTranslateProvider._map_api_error_code(code).value})
for code in ['AuthFailure.SecretIdNotFound', 'UnauthorizedOperation', 'FailedOperation.UserNotRegistered',
             'FailedOperation.NoFreeAmount', 'FailedOperation.ServiceIsolate', 'FailedOperation.StopUsing',
             'UnsupportedOperation.UnSupportedTargetLanguage', 'UnsupportedOperation.UnsupportedLanguage',
             'UnsupportedOperation.UnsupportedSourceLanguage', 'UnsupportedOperation.TextTooLong', 'RequestLimitExceeded',
             'LimitExceeded', 'FailedOperation.LimitedAccessFrequency', 'InvalidParameter', 'MissingParameter',
             'InvalidParameterValue', 'InternalError', 'InternalError.BackendTimeout', 'Something.Else', '',
             'AuthFailure.NoFreeAmount', 'FailedOperation.ServiceIsolate.Extra', 'FailedOperation.UserNotRegisteredX']:
    mt_error_codes.append({'provider': 'tencent', 'code': code, 'expected': TencentTranslateProvider._map_api_error_code(code).value})

mt_language_codes = []
for code in ['zh-Hans', 'zh-Hant', 'en-US', 'en-GB', 'pt-BR', 'pt-PT', 'ja', 'ko', 'fr', '', 'zh', 'zh-TW', 'nl', 'en']:
    for provider_id, (provider_class, _) in MT.items():
        mt_language_codes.append({'provider': provider_id, 'code': code, 'to': provider_class._to_code(code),
                                  'from': provider_class._from_code(code)})


# The reference accessors bound to a throwaway QSettings, never the user's.
class QuotaSettings:
    APP_DEFAULT_SETTINGS = ToolSettingsManager.APP_DEFAULT_SETTINGS

    def __init__(self, qsettings):
        self.qsettings = qsettings


for provider_id in MT:
    for name in ('get_{}_translate_quota_enabled', 'set_{}_translate_quota_enabled', 'get_{}_translate_quota_limit',
                 'set_{}_translate_quota_limit'):
        setattr(QuotaSettings, name.format(provider_id), getattr(ToolSettingsManager, name.format(provider_id)))
QuotaSettings.get_translation_quota_enabled = ToolSettingsManager.get_translation_quota_enabled
QuotaSettings.get_translation_quota_limit = ToolSettingsManager.get_translation_quota_limit

INT64_MIN, INT64_MAX = -2 ** 63, 2 ** 63 - 1
quota_settings = []
with tempfile.TemporaryDirectory() as temp:
    qsettings = QSettings(str(Path(temp) / 'quota.ini'), QSettings.IniFormat)
    quota = QuotaSettings(qsettings)
    for provider_id in [*MT, 'deepl', ' VolcEngine ', '']:
        quota_settings.append({'op': 'read', 'provider': provider_id, 'enabled': quota.get_translation_quota_enabled(provider_id),
                               'limit': quota.get_translation_quota_limit(provider_id)})
    for provider_id in MT:
        for value in ['', ' 1234 ', 'abc', '0', '-5', '+7', '1_000', '1__0', '_1', '1_', '+_1', '+', '１２３', '٣٤',
                      '\u3000 42 \u3000', '1.5', '1e3', '9' * 25, '-' + '9' * 25, '9223372036854775807', '9223372036854775808']:
            getattr(quota, f'set_{provider_id}_translate_quota_limit')(value)
            # Python ints are unbounded; the C++ port saturates at qint64.
            limit = min(max(quota.get_translation_quota_limit(provider_id), INT64_MIN), INT64_MAX)
            quota_settings.append({'op': 'limit', 'provider': provider_id, 'input': value,
                                   'stored': getattr(quota, f'get_{provider_id}_translate_quota_limit')(), 'limit': limit})
        for value in [False, True]:
            getattr(quota, f'set_{provider_id}_translate_quota_enabled')(value)
            quota_settings.append({'op': 'enabled', 'provider': provider_id, 'input': value,
                                   'enabled': quota.get_translation_quota_enabled(provider_id)})
    del quota, qsettings

fixture = {'html_unescape': unescape, 'extract': extract, 'normalize_base_url': normalize, 'parse_response': parse,
           'custom_llm': custom, 'amazon_signature': amazon, 'baidu_sign': baidu, 'urlencode': urlencode,
           'mt_metadata': mt_metadata, 'volcengine_signature': volcengine_signature, 'tencent_signature': tencent_signature,
           'mt_requests': mt_requests, 'mt_responses': mt_responses, 'mt_prechecks': mt_prechecks,
           'mt_error_codes': mt_error_codes, 'mt_language_codes': mt_language_codes, 'quota_settings': quota_settings}
target = ROOT / 'tests' / 'fixtures' / 'translation.json'
target.write_text(json.dumps(fixture, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
print(f"{len(unescape)} unescape, {len(parse)} parse, {len(custom)} custom, {len(mt_responses)} mt responses, "
      f"{len(quota_settings)} quota settings -> {target.relative_to(ROOT)}")
