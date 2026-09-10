#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NextOS: check editorial language pairs, local links and shell syntax.

This does not execute documented commands or certify semantic translations.
PT: confere pares, links e sintaxe; não executa comandos nem certifica tradução.
"""
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
FENCE = re.compile(r'^```([^\n]*)\n(.*?)^```\s*$', re.M | re.S)
LINK = re.compile(r'(?<!!)\[[^\]\n]+\]\(([^\s)]+)\)')
EXECUTABLE_LANGUAGES = {'sh', 'bash', 'python', 'c', 'cmake', 'json'}


def main():
    config = json.loads((ROOT / 'publication/languages.json').read_text())
    errors, seen = [], set()
    link_count = shell_count = 0
    for pair in config['pairs']:
        bodies = {}
        for language in ('pt-BR', 'en'):
            relative = pair[language]
            if relative in seen:
                errors.append('Duplicate editorial path: ' + relative)
            seen.add(relative)
            path = ROOT / relative
            if not path.is_file():
                errors.append('Missing translation: ' + relative)
                continue
            body = path.read_text()
            bodies[language] = body
            if not body.startswith('# ') or len(body.split()) < 40:
                errors.append('Empty or incomplete editorial page: ' + relative)
            if len(re.findall(r'^```', body, re.M)) % 2:
                errors.append('Unclosed code fence: ' + relative)
            prose = FENCE.sub('', body)
            targets = []
            for target in LINK.findall(prose):
                parsed = urlsplit(target)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                dest = (path.parent / unquote(parsed.path)).resolve()
                try:
                    dest.relative_to(ROOT)
                    inside = True
                except ValueError:
                    inside = False
                if not inside or not dest.exists():
                    errors.append('Broken local link: %s -> %s' % (relative, target))
                targets.append(dest)
                link_count += 1
            other = pair['en' if language == 'pt-BR' else 'pt-BR']
            if (ROOT / other).resolve() not in targets:
                errors.append('Missing language switch: ' + relative)
            for kind, code in FENCE.findall(body):
                if kind.strip() in {'sh', 'bash'}:
                    result = subprocess.run(['bash', '-n'], input=code, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    shell_count += 1
                    if result.returncode:
                        errors.append('Shell syntax: ' + relative + ': ' + result.stderr.strip())
        if len(bodies) == 2:
            code = {lang: [(kind.strip(), text) for kind, text in FENCE.findall(body)
                           if kind.strip() in EXECUTABLE_LANGUAGES]
                    for lang, body in bodies.items()}
            if code['pt-BR'] != code['en']:
                errors.append('Executable example differs between languages: ' + pair['pt-BR'])
            headings = {lang: len(re.findall(r'^## ', body, re.M)) for lang, body in bodies.items()}
            if headings['pt-BR'] != headings['en']:
                errors.append('Section count differs: ' + pair['pt-BR'])
    # Pinned historical documents are not editorial translations.
    for p in ROOT.rglob('*.md'):
        relative = p.relative_to(ROOT)
        if relative.parts[0] in {'.git', 'work', 'framework', 'suportando_outros_devices'}:
            continue
        if relative.parts[0] == 'ports' and len(relative.parts) > 2 and relative.parts[2] == 'upstream':
            continue
        if str(relative) not in seen:
            errors.append('Editorial page has no registered language pair: ' + str(relative))
    if errors:
        print('\n'.join(errors))
        return 1
    print('PASS: %d complete language pairs; %d local links; %d shell blocks parse; executable examples match' %
          (len(config['pairs']), link_count, shell_count))
    print('Scope: structural consistency; manual translation review still matters. Commands were not executed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
