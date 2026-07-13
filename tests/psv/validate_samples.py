#!/usr/bin/env python3
"""Validate JSONL PSV samples (stdin) against the spec's JSON Schema (argv[1]).

Independent check of the C++ serializer: the schema file is copied verbatim
from spec PRISM-SPEC-PSV-001 section 5.2 and interpreted by the reference
python-jsonschema implementation, not by our own code.
"""
import json
import sys
from pathlib import Path

import jsonschema

schema = json.loads(Path(sys.argv[1]).read_text())
validator = jsonschema.Draft202012Validator(schema)

count = 0
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    validator.validate(json.loads(line))
    count += 1

if count == 0:
    sys.exit("no samples received on stdin — emitter failed?")
print(f"{count} PSV samples validate against the spec schema")
