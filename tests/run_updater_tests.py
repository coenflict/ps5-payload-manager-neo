#!/usr/bin/env python3
"""Automatic updates against a local GitHub-API/catalog/asset fixture.

Compiles the actual updater, upstream resolver, boot guard, release channel,
repository, sources and slot storage. Only the network-presence probe is
replaced. Covers upstream-first updates, catalog fallback, ETag caching, rate
limits, checksum/ELF rejection, policies, approval, rollback, the boot budget,
offline boots, crash rollback and fork-safe self-update channels."""
import argparse
import hashlib
import sys
import http.server
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--thread-sanitize', action='store_true')
args = parser.parse_args()
os.environ.setdefault('TSAN_OPTIONS', 'halt_on_error=1')
os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=1:halt_on_error=1')
os.environ.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
CHANNEL = 'tester/pldmgr-fork'
checks = 0


def check(condition, message=''):
    global checks
    checks += 1
    assert condition, message


def elf(body, *markers):
    header = bytearray(64)
    header[0:4] = b'\x7fELF'
    header[4], header[5], header[18] = 2, 1, 62
    return bytes(header) + body + b''.join(m + b'\0' for m in markers)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.routes, self.requests, self.lock = {}, [], threading.Lock()

    def route(self, path, body=b'', status=200, headers=None, etag=None, delay=0):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.routes[path] = dict(body=body, status=status, headers=headers or {}, etag=etag, delay=delay)

    def count(self, prefix):
        with self.lock:
            return sum(1 for path, _ in self.requests if path.startswith(prefix))

    def last(self, prefix):
        with self.lock:
            return [headers for path, headers in self.requests if path.startswith(prefix)][-1]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        server = self.server
        with server.lock:
            server.requests.append((self.path, dict(self.headers)))
        route = server.routes.get(self.path)
        if not route:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        if route['delay']:
            time.sleep(route['delay'])
        if route['etag'] and self.headers.get('If-None-Match') == route['etag']:
            self.send_response(304); self.send_header('ETag', route['etag']); self.end_headers(); return
        self.send_response(route['status'])
        for key, value in route['headers'].items():
            self.send_header(key, value)
        if route['etag']:
            self.send_header('ETag', route['etag'])
        self.send_header('Content-Length', str(len(route['body'])))
        self.end_headers()
        self.wfile.write(route['body'])


server = Fixture()
threading.Thread(target=server.serve_forever, daemon=True).start()
BASE = f'http://127.0.0.1:{server.server_address[1]}'
CATALOG_URL = BASE + '/catalog.json'

with tempfile.TemporaryDirectory(prefix='pldmgr-updater-') as temp:
    build = Path(temp)
    data = build / 'data'
    payloads = data / 'payloads'
    payloads.mkdir(parents=True)
    header = (ROOT / 'include/pldmgr.h').read_text().replace('/data/pldmgr', str(data))
    header = re.sub(r'(#define REPOSITORY_SOURCE_URL\s*\\\s*)"[^"]+"', lambda m: m.group(1) + '"' + CATALOG_URL + '"', header)
    (build / 'pldmgr.h').write_text(header)
    (build / 'assets_cacert_pem.h').write_text('static const unsigned char assets_cacert_pem[]={0};\nstatic const unsigned int assets_cacert_pem_len=0;\n')
    running = re.search(r'#define MENU_VERSION "([^"]+)"', header)[1]
    flags = shlex.split(os.environ.get('HOST_CC', 'clang')) + [
        '-std=c11', '-D_DEFAULT_SOURCE', '-g', '-O1', '-Wall', '-Wextra', '-pthread',
        '-ffunction-sections', '-fdata-sections', '-fno-omit-frame-pointer',
        '-fsanitize=thread' if args.thread_sanitize else '-fsanitize=address,undefined',
        '-I' + str(build), '-Iinclude', f'-DPLDMGR_GITHUB_API="{BASE}/gh"', f'-DPLDMGR_UPDATE_REPO="{CHANNEL}"',
        '-DPLDMGR_NETWORK_WAIT_MS=300', '-DBOOT_GUARD_PROBATION_SECONDS=1']
    objects = []
    for name in ['updater', 'upstream', 'boot_guard', 'channel', 'repository', 'sources', 'slot',
                 'utils', 'json_helpers', 'sha256', 'config']:
        extra = ['-Dpldmgr_get_local_ip=updater_test_ip'] if name == 'updater' else []
        obj = build / (name + '.o')
        subprocess.run(flags + extra + ['-c', f'src/{name}.c', '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = build / 'updater_tests'
    subprocess.run(flags + objects + ['tests/updater_driver.c', '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections', '-lcurl', '-o', str(binary)], cwd=ROOT, check=True)

    check(b'PLDMGR_CHANNEL:' + CHANNEL.encode() + b'\0' in binary.read_bytes(), 'channel marker survives --gc-sections')

    def run(*args, env=None, expect=0):
        result = subprocess.run([str(binary), *map(str, args)], env={**os.environ, **(env or {})},
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == expect, result.stdout + result.stderr
        return result

    def status(*args, **kw):
        result = run(*args, **kw)
        out = result.stdout
        start = out.index('{')
        end = out.rindex('}') + 1
        return json.loads(out[start:end]), out[end:] + result.stderr

    def items(doc):
        return {item['slot']: item for item in doc['items']}

    def slot_hash(url):
        return sha(url.strip().rstrip('/').encode())[:8]

    def install(kind, leaf, filename, content, **meta):
        slot = payloads / kind / leaf
        slot.mkdir(parents=True)
        (slot / filename).write_bytes(content)
        (slot / 'meta.json').write_text(json.dumps({'schema': 2, 'kind': kind, 'filename': filename, **meta}))
        return f'{kind}/{leaf}'

    def meta(key):
        return json.loads((payloads / key / 'meta.json').read_text())

    def payload_bytes(key):
        m = meta(key)
        return (payloads / key / m['filename']).read_bytes()

    # ── Pure resolver behaviour ───────────────────────────────
    for text, expected in [
        ('EchoStretch/kstuff-lite', '0 1 EchoStretch kstuff-lite -'),
        ('github.com/ps5-payload-dev/elfldr', '0 1 ps5-payload-dev elfldr -'),
        ('https://github.com/drakmor/ShadowMountPlus/releases/download/1.7/x.zip', '0 1 drakmor ShadowMountPlus -'),
        ('https://github.com/owner/repo.git', '0 1 owner repo -'),
        ('https://git.etawen.dev/earthonion/garlic-savemgr/releases', '0 2 earthonion garlic-savemgr https://git.etawen.dev'),
        ('http://github.com/owner/repo', '-1 0 - - -'), ('owner', '-1 0 - - -'), ('a/b/c', '-1 0 - - -'),
        ('https://user@evil/owner/repo', '-1 0 - - -'), ('../x/repo', '-1 0 - - -')]:
        check(run('parse', text).stdout.strip() == expected, (text, run('parse', text).stdout))
    for asset, version, expected in [
        ('https://github.com/x/y/releases/download/v7.0.31/ps5upload-7.0.31.elf', 'v7.0.31', '0 ps5upload-*.elf'),
        ('https://github.com/x/y/releases/download/v1.11/kstuff.elf', 'v1.11', '0 kstuff.elf'),
        ('https://github.com/x/y/releases/download/2.5B/etaHEN-2.5B.bin', '2.5B', '0 etaHEN-*.bin'),
        ('https://github.com/x/y/releases/download/1.3.2/ps5debug-NG_v1.3.2.elf', '1.3.2', '0 ps5debug-NG_v*.elf'),
        ('https://github.com/x/y/releases/download/v1.6.0/zftpd-ps5-zhttp-v1.6.0.elf', 'v1.6.0', '0 zftpd-ps5-zhttp-*.elf'),
        ('https://github.com/x/y/releases/download/1.7/SMP_1.7.zip', '1.7', '-1 -'),
        ('README.md', 'v1', '-1 -')]:
        check(run('derive', asset, version).stdout.strip() == expected, (asset, run('derive', asset, version).stdout))
    for pattern, name, expected in [('ps5upload-*.elf', 'PS5Upload-7.1.elf', '1'), ('kstuff.elf', 'kstuff.elf.sig', '0'),
                                    ('*', 'anything.bin', '1'), ('a*b*c', 'axxbyyc', '1'), ('a*b', 'ac', '0')]:
        check(run('glob', pattern, name).stdout.strip() == expected, (pattern, name))
    for version, expected in [('v1.11', '1'), ('0.20', '0'), ('2.5B', '2'), ('38108724867', '-1'), ('beta', '-1')]:
        check(run('major', version).stdout.strip() == expected, version)
    digest = 'sha256:' + 'AB' * 32
    releases = [
        {'tag_name': 'v3.0', 'draft': True, 'prerelease': False, 'assets': [{'name': 'tool-3.0.elf', 'browser_download_url': BASE + '/d', 'digest': digest}]},
        {'tag_name': 'v2.1-beta', 'draft': False, 'prerelease': True, 'assets': [{'name': 'tool-2.1-beta.elf', 'browser_download_url': BASE + '/d', 'digest': digest}]},
        {'tag_name': 'v1.10', 'draft': False, 'prerelease': False, 'published_at': '2026-10-01T00:00:00Z',
         'assets': [{'name': 'tool-1.10.elf', 'browser_download_url': BASE + '/a', 'digest': digest, 'size': 9},
                    {'name': 'tool-1.10-debug.elf', 'browser_download_url': BASE + '/b', 'digest': None}]},
        {'tag_name': 'v1.9', 'draft': False, 'prerelease': False, 'assets': [{'name': 'tool-1.9.elf', 'browser_download_url': BASE + '/c'}]},
        {'tag_name': 'v1.12', 'draft': False, 'prerelease': False, 'assets': [{'name': 'tool-1.12.zip', 'browser_download_url': BASE + '/z'}]},
    ]
    sample = build / 'releases.json'
    sample.write_text(json.dumps(releases))
    check(run('select', sample, 'tool-*.elf').stdout.split() == ['0', 'v1.10', 'tool-1.10.elf', 'ab' * 32], run('select', sample, 'tool-*.elf').stdout)
    check(run('select', sample, 'tool-*.elf', '', 'pre').stdout.split()[1] == 'v2.1-beta')
    check(run('select', sample, 'tool-*.elf', 'debug').stdout.split()[:3] == ['0', 'v1.10', 'tool-1.10-debug.elf'])
    check(run('select', sample, 'other-*.elf').stdout.split()[0] == '1')
    releases.insert(0, {'tag_name': 'v1.11-beta2', 'draft': False, 'prerelease': False,
                        'assets': [{'name': 'tool-1.11-beta2.elf', 'browser_download_url': BASE + '/e', 'digest': digest}]})
    sample.write_text(json.dumps(releases))
    check(run('select', sample, 'tool-*.elf').stdout.split()[1] == 'v1.10', 'unflagged beta tag is a prerelease')
    check(run('select', sample, 'tool-*.elf', '', 'pre').stdout.split()[1] == 'v2.1-beta')
    for version, expected in [('1.7-beta5fix1', True), ('2.5B', False), ('v1.0-rc1', True), ('1.16-ng-stable', False),
                              ('v0.9.0', False), ('nightly-20261001', True), ('38108724867', False)]:
        result = run('prerelease', version).stdout.strip()
        check(result == ('1' if expected else '0'), (version, result))
    sample.write_text('{"message":"Not Found"}')
    check(run('select', sample, '*').stdout.split()[0] == '-1')

    # ── Fixture world ─────────────────────────────────────────
    K = {v: elf(f'kstuff {v}'.encode()) for v in ['v1.11', 'v1.12', 'v1.13', 'v1.14', 'v2.0', 'v2.1']}
    SMP16, SMP17 = elf(b'smp 1.6'), elf(b'smp 1.7')
    LOCAL = elf(b'local upload')
    api = '/gh/repos/EchoStretch/kstuff-lite/releases?per_page=20'

    def catalog(kstuff_version):
        server.route('/catalog.json', {'name': 'Fixture', 'payloads': [
            {'name': 'kstuff-lite', 'filename': f'kstuff-lite_{kstuff_version}.elf', 'url': f'{BASE}/mirror/kstuff-{kstuff_version}.elf',
             'version': kstuff_version, 'checksum': sha(K[kstuff_version]), 'last_update': '2026-09-20',
             'source': 'https://github.com/EchoStretch/kstuff-lite/releases',
             'source_direct': 'https://github.com/EchoStretch/kstuff-lite/releases/download/v1.11/kstuff.elf'},
            {'name': 'ShadowMountPlus', 'filename': 'ShadowMountPlus_1.7.elf', 'url': f'{BASE}/mirror/smp-1.7.elf',
             'version': '1.7', 'checksum': sha(SMP17), 'extract_file': 'shadowmountplus.elf',
             'source': 'https://github.com/drakmor/ShadowMountPlus/releases',
             'source_direct': 'https://github.com/drakmor/ShadowMountPlus/releases/download/1.7/SMP_1.7.zip'}]})
        server.route(f'/mirror/kstuff-{kstuff_version}.elf', K[kstuff_version])
        (data / ('repo_cache_' + slot_hash(CATALOG_URL) + '.json.ts')).unlink(missing_ok=True)

    def upstream(*versions, etag=None, bad_digest=(), html=(), delay=0):
        body = []
        for version in versions:
            content = b'<html>not a payload</html>' if version in html else K[version]
            name = 'kstuff.elf'
            url = f'{BASE}/dl/{version}/{name}'
            server.route(f'/dl/{version}/{name}', content, delay=delay)
            body.append({'tag_name': version, 'draft': False, 'prerelease': False, 'published_at': '2026-10-15T10:00:00Z',
                         'assets': [{'name': name, 'browser_download_url': url, 'size': len(content),
                                     'digest': 'sha256:' + (sha(b'tampered') if version in bad_digest else sha(content))}]})
        body.append({'tag_name': 'v9.0-beta', 'draft': False, 'prerelease': True,
                     'assets': [{'name': 'kstuff.elf', 'browser_download_url': f'{BASE}/never', 'digest': 'sha256:' + sha(b'x')}]})
        server.route(api, body, etag=etag)

    h = slot_hash(CATALOG_URL)
    kstuff = install('repo', f'kstuff-lite@{h}', 'kstuff-lite_v1.11.elf', K['v1.11'], id='kstuff-lite', source_url=CATALOG_URL,
                     name='kstuff-lite', version='v1.11', checksum=sha(K['v1.11']),
                     source='https://github.com/EchoStretch/kstuff-lite/releases')
    smp = install('repo', f'shadowmountplus@{h}', 'ShadowMountPlus_1.6.elf', SMP16, id='shadowmountplus', source_url=CATALOG_URL,
                  name='ShadowMountPlus', version='1.6', checksum=sha(SMP16))
    local = install('local', 'manual', 'manual.elf', LOCAL, origin='web_upload')
    (data / 'autoload.txt').write_text(f'{kstuff}\n!500\nShadowMountPlus_1.6.elf\n{local}\n')
    (data / 'favorites.txt').write_text('ShadowMountPlus_1.6.elf\n')
    catalog('v1.11')
    server.route('/mirror/smp-1.7.elf', SMP17)
    upstream('v1.12', 'v1.11', etag='"r1"')

    # Boot pass: upstream release newer than the mirror wins, archive-only
    # upstream falls back to the catalog, entries become ready in order.
    # The bare entry is waited for after its update renamed the file (items
    # run in sequence order, so local/manual finishes after ShadowMountPlus).
    doc, waits = status('pass', kstuff, local, 'ShadowMountPlus_1.6.elf', '!500')
    state = items(doc)
    check(state[kstuff]['state'] == 'updated' and meta(kstuff)['version'] == 'v1.12', state[kstuff])
    check(payload_bytes(kstuff) == K['v1.12'] and meta(kstuff)['checksum'] == sha(K['v1.12']))
    check(meta(kstuff)['filename'] == 'kstuff-lite_v1.12.elf' and 'upstream release' in state[kstuff]['message'])
    check(state[kstuff]['upstream'] == 'EchoStretch/kstuff-lite' and state[kstuff]['pattern'] == 'kstuff.elf')
    check(state[kstuff]['has_previous'] and state[kstuff]['previous'] == 'v1.11')
    previous = payloads / kstuff / '.previous'
    check((previous / 'kstuff-lite_v1.11.elf').read_bytes() == K['v1.11'])
    check(state[smp]['state'] == 'updated' and meta(smp)['version'] == '1.7' and payload_bytes(smp) == SMP17, state[smp])
    check('catalog' in state[smp]['message'])
    autoload = (data / 'autoload.txt').read_text().split()
    check(autoload == [kstuff, '!500', smp, local], autoload)
    check((data / 'favorites.txt').read_text().split() == [smp])
    check(state[local]['state'] == 'untracked' and state[local]['autoload'], state[local])
    check(f'WAIT {kstuff} -> {kstuff} rc=0' in waits and f'WAIT ShadowMountPlus_1.6.elf -> {smp} rc=0' in waits, waits)
    check('WAIT !500 ->  rc=0' in waits)
    check(server.count('/never') == 0 and server.count(api) == 1)
    check(not list(payloads.glob('.download-*')))

    # Second boot within the recheck window: no API request at all.
    doc, _ = status('pass', kstuff)
    check(items(doc)[kstuff]['state'] == 'current' and server.count(api) == 1)
    # Manual check: conditional request answered by 304, which GitHub does not
    # count against the rate limit.
    doc, _ = status('manual')
    check(server.count(api) == 2 and server.last(api).get('If-None-Match') == '"r1"')
    check(server.last(api).get('Accept') == 'application/vnd.github+json')
    check(items(doc)[kstuff]['state'] == 'current')

    # Rate limit: upstream pauses, the catalog still delivers its newer version.
    server.route(api, {'message': 'API rate limit exceeded'}, status=403,
                 headers={'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(int(time.time()) + 3600)})
    catalog('v1.13')
    doc, _ = status('manual')
    state = items(doc)[kstuff]
    check(meta(kstuff)['version'] == 'v1.13' and payload_bytes(kstuff) == K['v1.13'] and state['state'] == 'updated', state)
    requests = server.count(api)
    doc, _ = status('manual')
    check(server.count(api) == requests, 'rate-limited API must not be retried before reset')
    (data / 'upstream' / 'upstream_ratelimit').unlink()

    # Tampered upstream asset: discarded; the catalog's verified copy is used.
    catalog('v1.14')
    upstream('v1.14', bad_digest=('v1.14',), etag='"r2"')
    doc, _ = status('manual')
    state = items(doc)[kstuff]
    check(meta(kstuff)['version'] == 'v1.14' and payload_bytes(kstuff) == K['v1.14'], state)
    check(state['state'] == 'updated' and 'catalog' in state['message'], state)
    check(not list(payloads.glob('.download-*')))

    # A non-payload (HTML) asset with a matching digest is still rejected.
    K['v1.15'] = b'<html>not a payload</html>'
    upstream('v1.15', html=('v1.15',), etag='"r3"')
    doc, _ = status('manual')
    state = items(doc)[kstuff]
    check(state['state'] == 'failed' and 'not a PS5 payload' in state['message'], state)
    check(meta(kstuff)['version'] == 'v1.14' and payload_bytes(kstuff) == K['v1.14'])

    # Major update under the default "safe" policy waits for approval.
    upstream('v2.0', etag='"r4"')
    doc, _ = status('manual')
    state = items(doc)[kstuff]
    check(state['state'] == 'approval' and state['latest'] == 'v2.0' and meta(kstuff)['version'] == 'v1.14', state)
    out = run('apply', kstuff).stdout
    check('RESULT 0' in out and meta(kstuff)['version'] == 'v2.0' and payload_bytes(kstuff) == K['v2.0'], out)

    # Rollback restores the retained version and skips the replaced one.
    out = run('rollback', kstuff).stdout
    check('RESULT 0' in out and meta(kstuff)['version'] == 'v1.14' and payload_bytes(kstuff) == K['v1.14'], out)
    check((payloads / kstuff / '.previous' / 'kstuff-lite_v2.0.elf').exists())
    doc, _ = status('manual')
    state = items(doc)[kstuff]
    check(state['skip'] == 'v2.0' and state['state'] == 'current' and meta(kstuff)['version'] == 'v1.14', state)

    # Policies and validation.
    check(run('settings', json.dumps({'payloads': {kstuff: {'policy': 'notify', 'skip': ''}}})).stdout.strip() == '0')
    doc, _ = status('manual')
    check(items(doc)[kstuff]['state'] == 'available' and meta(kstuff)['version'] == 'v1.14')
    check(run('settings', json.dumps({'payloads': {kstuff: {'policy': 'off'}}})).stdout.strip() == '0')
    doc, _ = status('manual')
    check(items(doc)[kstuff]['state'] == 'disabled')
    for bad in [{'budget': 999}, {'enabled': 'yes'}, {'payloads': {kstuff: {'policy': 'always'}}},
                {'payloads': {'../etc': {}}}, {'payloads': {kstuff: {'repo': 'not a repo'}}},
                {'payloads': {kstuff: {'asset': 'a/b'}}}]:
        check(run('settings', json.dumps(bad)).stdout.strip() == '-2', bad)
    saved = json.loads((data / 'updates.json').read_text())
    check(saved['payloads'][kstuff]['policy'] == 'off' and saved['budget'] == 20)

    # Boot budget: a slow download never holds the sequence beyond the budget;
    # the update still lands for the next boot.
    check(run('settings', json.dumps({'budget': 1, 'payloads': {kstuff: {'policy': 'auto'}}})).stdout.strip() == '0')
    upstream('v2.1', etag='"r5"', delay=3)
    for cached in (data / 'upstream').glob('upstream_*'):
        cached.unlink()
    doc, waits = status('pass', kstuff)
    wait = re.search(rf'WAIT {re.escape(kstuff)} -> \S+ rc=(\d) ms=(\d+)', waits)
    check(wait and wait.group(1) == '1' and int(wait.group(2)) < 2500, waits)
    check(meta(kstuff)['version'] == 'v2.1' and items(doc)[kstuff]['state'] == 'updated', 'update lands after the budget')

    # Offline boot: no waiting, installed payloads stay.
    before = meta(kstuff)['version']
    doc, waits = status('pass', kstuff, env={'UPDATER_OFFLINE': '1'})
    wait = re.search(r'rc=(\d) ms=(\d+)', waits)
    check(wait.group(1) == '0' and int(wait.group(2)) < 1500, waits)
    check(items(doc)[kstuff]['state'] == 'offline' and doc['network'] == 'offline' and meta(kstuff)['version'] == before)

    # Disabled: nothing is checked or waited for.
    check(run('settings', json.dumps({'enabled': False})).stdout.strip() == '0')
    requests = server.count('/')
    doc, waits = status('pass', kstuff)
    check(items(doc)[kstuff]['state'] == 'disabled' and 'rc=0' in waits)
    check(run('settings', json.dumps({'enabled': True, 'budget': 20})).stdout.strip() == '0')

    # ── Boot guard ────────────────────────────────────────────
    current = meta(kstuff)['version']
    prior = json.loads((payloads / kstuff / '.previous' / 'meta.json').read_text())['version']
    run('guard-launch', kstuff, current)
    check(run('guard-eval', '1').stdout.strip() == '0' and meta(kstuff)['version'] == current, 'replaced instance is not a crash')
    run('guard-launch', kstuff, current, 'healthy')
    check(run('guard-eval', '0').stdout.strip() == '0' and meta(kstuff)['version'] == current, 'healthy boot keeps the update')
    run('guard-launch', smp, '0.0.0-not-installed')
    check(run('guard-eval', '0').stdout.strip() == '0' and meta(smp)['version'] == '1.7', 'only the exact installed version is undone')
    run('guard-launch', kstuff, current, 'done')  # crash during probation
    check(run('guard-eval', '0').stdout.strip() == '1' and meta(kstuff)['version'] == prior)
    check(json.loads((data / 'updates.json').read_text())['payloads'][kstuff]['skip'] == current)
    check('state=clean' in (data / 'boot_guard.txt').read_text())
    check(run('guard-eval', '0').stdout.strip() == '0')

    # ── Release channel ───────────────────────────────────────
    fork = lambda v: elf(b'manager', b'PLDMGR_VER:' + v.encode(), b'PLDMGR_CHANNEL:' + CHANNEL.encode())
    official = lambda v: elf(b'manager', b'PLDMGR_VER:' + v.encode())
    selfdir = data / 'self'
    selfdir.mkdir()
    (selfdir / 'pldmgr_v0.9.0.elf').write_bytes(fork('0.9.0'))
    (selfdir / 'pldmgr_v1.0.0.elf').write_bytes(official('1.0.0'))
    (selfdir / 'pldmgr_v2.0.0.elf').write_bytes(elf(b'manager', b'PLDMGR_VER:2.0.0', b'PLDMGR_CHANNEL:someone/else'))
    (payloads / 'pldmgr').mkdir()
    (payloads / 'pldmgr' / 'pldmgr_v5.0.0.elf').write_bytes(official('5.0.0'))
    check(run('fd-matches', selfdir / 'pldmgr_v0.9.0.elf').stdout.strip() == '1')
    check(run('fd-matches', selfdir / 'pldmgr_v1.0.0.elf').stdout.strip() == '0')
    check(run('handoff').stdout.split()[:2] == ['1', '0.9.0'], 'fork never hands off to upstream or another fork')
    run('attempt', '0.9.0')
    check(run('handoff').stdout.split()[0] == '0', 'a build that never came up is skipped')
    check('failed=0.9.0' in (selfdir / 'state.txt').read_text())
    (selfdir / 'state.txt').write_text(f'attempt={running}\nhealthy=\nfailed=0.9.0\n')
    run('healthy')
    check(f'healthy={running}' in (selfdir / 'state.txt').read_text() and 'attempt=\n' in (selfdir / 'state.txt').read_text())
    channel_api = f'/gh/repos/{CHANNEL}/releases?per_page=20'
    good = fork('0.8.0')
    server.route('/dl/manager-0.8.0', good)
    server.route(channel_api, [{'tag_name': 'v0.8.0', 'draft': False, 'prerelease': False,
                                'assets': [{'name': 'pldmgr_v0.8.0.elf', 'browser_download_url': BASE + '/dl/manager-0.8.0',
                                            'digest': 'sha256:' + sha(good)}]}])
    out = run('channel-check').stdout.strip().split('|')
    check(out[0] == 'ready' and out[1] == 'v0.8.0' and out[2] == '0.8.0' and out[4] == '0', out)
    check((selfdir / 'pldmgr_v0.8.0.elf').read_bytes() == good)
    check(not (selfdir / 'pldmgr_v1.0.0.elf').exists() and (payloads / 'pldmgr' / 'pldmgr_v5.0.0.elf').exists())
    check(run('handoff').stdout.split()[:2] == ['1', '0.8.0'])
    impostor = official('0.8.1')
    server.route('/dl/manager-0.8.1', impostor)
    server.route(channel_api, [{'tag_name': 'v0.8.1', 'draft': False, 'prerelease': False,
                                'assets': [{'name': 'pldmgr_v0.8.1.elf', 'browser_download_url': BASE + '/dl/manager-0.8.1',
                                            'digest': 'sha256:' + sha(impostor)}]}])
    out = run('channel-check').stdout.strip().split('|')
    check(out[0] == 'error' and 'different update channel' in out[3] and not (selfdir / 'pldmgr_v0.8.1.elf').exists(), out)
    mislabeled = fork('0.8.3')
    server.route('/dl/manager-0.8.2', mislabeled)
    server.route(channel_api, [{'tag_name': 'v0.8.2', 'draft': False, 'prerelease': False,
                                'assets': [{'name': 'pldmgr_v0.8.2.elf', 'browser_download_url': BASE + '/dl/manager-0.8.2',
                                            'digest': 'sha256:' + sha(mislabeled)}]}])
    out = run('channel-check').stdout.strip().split('|')
    check(out[0] == 'error' and 'Embedded version' in out[3], out)

    # ── Server gone: every failure keeps the installed payloads ──
    snapshot = {key: payload_bytes(key) for key in [kstuff, smp, local]}
    server.shutdown(); server.server_close()
    doc, waits = status('manual')
    check(all(payload_bytes(key) == value for key, value in snapshot.items()))
    check(items(doc)[kstuff]['state'] in ('current', 'failed', 'disabled'), items(doc)[kstuff])
    check(not list(payloads.glob('.download-*')) and not list(selfdir.glob('.download-*')))
    print(f'Updater: {checks} checks passed (upstream-first, catalog fallback, ETag/304, rate limit, checksum/ELF '
          'rejection, policies, approval, rollback, boot budget, offline, crash guard, release channel)')
