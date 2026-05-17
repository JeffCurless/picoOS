#!/usr/bin/env python3
"""Prepend MIT + Commons Clause license header to all .c and .h files under src/."""

import os

HEADER = """\
/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */
"""

src_dir = os.path.join(os.path.dirname(__file__), '..', 'src')
updated = []
skipped = []

for root, _, files in os.walk(src_dir):
    for fname in sorted(files):
        if not (fname.endswith('.c') or fname.endswith('.h')):
            continue
        path = os.path.join(root, fname)
        with open(path, 'r', encoding='utf-8') as f:
            content = f.read()
        if 'MIT License with Commons Clause' in content:
            skipped.append(path)
            continue
        with open(path, 'w', encoding='utf-8') as f:
            f.write(HEADER + '\n' + content)
        updated.append(path)

for p in updated:
    print(f'  updated: {os.path.relpath(p)}')
for p in skipped:
    print(f'  skipped: {os.path.relpath(p)}')
print(f'\n{len(updated)} file(s) updated, {len(skipped)} skipped.')
