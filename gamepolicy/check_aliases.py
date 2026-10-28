#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host gate: check each overlay against its unmodified extracted policy."""
import sys
import xml.etree.ElementTree as ET


def check(stock, overlay):
    packages = {pkg.strip() for game in ET.parse(stock).findall('./GamePolicy/Game')
                for pkg in game.attrib['pkg'].split(',')}
    aliases = ET.parse(overlay).findall('./alias')
    alias_packages = {a.attrib['pkg'] for a in aliases}
    dangling = [a.attrib['pkg'] + ' -> ' + a.attrib['profile'] for a in aliases
                if a.attrib['profile'] not in packages]
    chained = [a.attrib['pkg'] + ' -> ' + a.attrib['profile'] for a in aliases
               if a.attrib['profile'] in alias_packages]
    shadowed = [a.attrib['pkg'] + ' -> ' + a.attrib['profile'] for a in aliases
                if a.attrib['pkg'] in packages]
    if shadowed:
        print('WARNING Shadowed aliases (own stock entry takes precedence): ' + ', '.join(shadowed))
    errors = []
    if dangling:
        errors.append('Dangling aliases: ' + ', '.join(dangling))
    if chained:
        errors.append('Chained aliases: ' + ', '.join(chained))
    if errors:
        raise ValueError('; '.join(errors))
    print(f'ALIASES_OK {overlay}: {len(aliases)} aliases, {len(packages)} packages')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: check_aliases.py STOCK_XML OVERLAY_XML')
    try:
        check(*sys.argv[1:])
    except (ValueError, OSError, ET.ParseError) as error:
        sys.exit(str(error))
