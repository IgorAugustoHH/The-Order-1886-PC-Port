#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
# SPDX-License-Identifier: GPL-2.0-or-later
here="$(cd -- "$(dirname -- "$0")" && pwd)"
exec python "$here/../../executar_cache.py" --threads 2
