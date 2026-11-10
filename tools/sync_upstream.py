#!/usr/bin/env python3
"""One-command upstream sync for this fork.

    python tools/sync_upstream.py            # fetch + rebase + auto-resolve + push
    python tools/sync_upstream.py --no-push # stop before force-pushing

This fork carries exactly two local changes:
  1. pin_config.h            - X179 wiring (BODY 4/5, CHASSIS 2/3, PARTY 26/27)
  2. dashboard_source.html   - Chinese i18n layer between P4CAN I18N markers

During `git rebase`, stage semantics are INVERTED relative to a normal merge:
  :2: ("ours")   = upstream/main + already replayed commits  <- use as BASE
  :3: ("theirs") = the local commit currently being replayed <- extract OUR values from here

Resolution rules:
  pin_config.h          : upstream text as base, overwrite the 6 CAN_* macros with our values
  dashboard_source.html : upstream text as base, re-attach our I18N block before </body>
  index_html.h          : generated - never merged by hand, always rebuilt by build_dashboard.py
  anything else         : abort and report (deliberate fail-safe, no guessing)
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
I18N_START = '<!-- P4CAN I18N START -->'
I18N_END = '<!-- P4CAN I18N END -->'
PIN_MACROS = (
    'CAN_BODY_TX', 'CAN_BODY_RX',
    'CAN_CHASSIS_TX', 'CAN_CHASSIS_RX',
    'CAN_PARTY_TX', 'CAN_PARTY_RX',
)
LOCAL_FILES = {'pin_config.h', 'dashboard_source.html', 'index_html.h'}
ENV = dict(os.environ, GIT_EDITOR='true', GIT_SEQUENCE_EDITOR='true')


def git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ['git', *args], cwd=ROOT, env=ENV, text=True,
        encoding='utf-8', errors='replace', capture_output=True, check=False,
    ) if not check else _checked(['git', *args])


def _checked(cmd: list[str]) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, cwd=ROOT, env=ENV, text=True,
                       encoding='utf-8', errors='replace', capture_output=True)
    if r.returncode:
        raise RuntimeError(f"{' '.join(cmd)} failed:\n{r.stdout}{r.stderr}")
    return r


def stage(path: str, index: int) -> str:
    r = git('show', f':{index}:{path}', check=False)
    return r.stdout if r.returncode == 0 else ''


def conflicted() -> list[str]:
    r = git('diff', '--name-only', '--diff-filter=U', check=False)
    return [x.strip() for x in r.stdout.splitlines() if x.strip()]


def extract_pin_values(our_text: str) -> dict[str, str]:
    out = {}
    for name in PIN_MACROS:
        m = re.search(rf'^\s*#define\s+{name}\s+(\d+)\s*$', our_text, re.M)
        if not m:
            raise RuntimeError(f'cannot find #{name} in our pin_config.h')
        out[name] = m.group(1)
    return out


def resolve_pin_config() -> None:
    base = stage('pin_config.h', 2) or (ROOT / 'pin_config.h').read_text(encoding='utf-8')
    ours = stage('pin_config.h', 3)
    if not ours:
        raise RuntimeError('missing stage :3: pin_config.h')
    values = extract_pin_values(ours)
    for name, value in values.items():
        pattern = rf'(^\s*#define\s+{name}\s+)(\d+)(\s*$)'
        if re.search(pattern, base, re.M):
            base = re.sub(pattern, rf'\g<1>{value}\g<3>', base, flags=re.M)
        else:
            base += f'\n#define {name} {value}\n'
    (ROOT / 'pin_config.h').write_text(base, encoding='utf-8')
    print('  resolved pin_config.h (upstream base + our CAN pins)')


def extract_i18n_block(our_text: str) -> str:
    a, b = our_text.find(I18N_START), our_text.find(I18N_END)
    if a < 0 or b < 0 or b <= a:
        raise RuntimeError('P4CAN I18N markers not found in our dashboard_source.html')
    return our_text[a:b + len(I18N_END)]


def resolve_dashboard() -> None:
    base = stage('dashboard_source.html', 2) or (ROOT / 'dashboard_source.html').read_text(encoding='utf-8')
    ours = stage('dashboard_source.html', 3)
    block = extract_i18n_block(ours) if ours else ''
    if not block:
        # our commit did not touch this file; keep upstream text unchanged
        (ROOT / 'dashboard_source.html').write_text(base, encoding='utf-8')
        return
    # drop any pre-existing injected block (e.g. upstream already merged the feature)
    a, b = base.find(I18N_START), base.find(I18N_END)
    if a >= 0 and b > a:
        base = base[:a].rstrip() + '\n' + base[b + len(I18N_END):].lstrip('\n')
    marker = '</body>'
    idx = base.lower().rfind(marker)
    if idx < 0:
        raise RuntimeError('no </body> in upstream dashboard_source.html')
    merged = base[:idx].rstrip() + '\n' + block + '\n' + base[idx:]
    (ROOT / 'dashboard_source.html').write_text(merged, encoding='utf-8')
    print('  resolved dashboard_source.html (upstream base + our I18N block)')


def rebuild_dashboard() -> None:
    _checked([sys.executable, str(ROOT / 'tools' / 'build_dashboard.py')])
    print('  rebuilt index_html.h from dashboard_source.html')


def resolve_round() -> None:
    files = conflicted()
    print(f'  conflicts: {", ".join(files)}')
    unsupported = [f for f in files if f not in LOCAL_FILES]
    if unsupported:
        raise RuntimeError(
            'unsupported conflict(s), manual review required: ' + ', '.join(unsupported)
            + '\n  rebase is left in progress; inspect with `git status`, then `git rebase --abort`'
        )
    if 'dashboard_source.html' in files:
        resolve_dashboard()
    elif 'index_html.h' in files:
        (ROOT / 'index_html.h').write_text(stage('index_html.h', 2), encoding='utf-8')
    if 'pin_config.h' in files:
        resolve_pin_config()
    if 'dashboard_source.html' in files or 'index_html.h' in files:
        rebuild_dashboard()
    add = ['dashboard_source.html', 'index_html.h', 'pin_config.h']
    git('add', '--', *[f for f in add if f in files])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-push', action='store_true', help='do not force-push to origin')
    args = ap.parse_args()

    dirty = [l for l in git('status', '--porcelain').stdout.splitlines()
             if l[:2].strip() and not l.startswith('??')]
    if dirty:
        print('ERROR: tracked files have uncommitted changes; commit or stash first:')
        print('\n'.join(dirty))
        return 1

    print('[1/4] fetch upstream')
    _checked(['git', 'fetch', 'upstream'])

    base = git('merge-base', 'main', 'upstream/main').stdout.strip()
    head = git('rev-parse', 'upstream/main').stdout.strip()
    if base == head:
        print('Upstream has no new commits - nothing to sync.')
        return 0
    new_count = git('rev-list', '--count', f'{base}..{head}').stdout.strip()
    print(f'[2/4] rebase onto upstream/main ({new_count} new commit(s))')

    r = git('rebase', 'upstream/main', check=False)
    if r.returncode:
        rounds = 0
        while r.returncode:
            rounds += 1
            if rounds > 20 or not conflicted():
                print(r.stdout + r.stderr)
                print('ERROR: rebase failed; leaving state for manual inspection.')
                return 1
            resolve_round()
            r = git('rebase', '--continue', check=False)
        print(f'  rebase completed after {rounds} conflict round(s)')
    else:
        print('  clean rebase, no conflicts')

    replayed = git('log', '--oneline', 'upstream/main..main').stdout.strip()
    print('[3/4] local commits still on top:')
    print('\n'.join('  ' + x for x in replayed.splitlines()) or '  (none - fork now equals upstream)')

    # generated artifact must be consistent with the final source
    _checked([sys.executable, str(ROOT / 'tools' / 'build_dashboard.py')])
    if git('status', '--porcelain', '--', 'index_html.h').stdout.strip():
        print('ERROR: index_html.h out of sync after rebase; review manually.')
        return 1

    if args.no_push:
        print('[4/4] skipped push (--no-push)')
        return 0
    print('[4/4] force-push to origin/main')
    out = git('push', '-f', 'origin', 'main', check=False)
    print(out.stdout + out.stderr)
    if out.returncode:
        return 1
    print('Sync complete.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:
        print('ERROR:', e)
        sys.exit(1)
