Proves a malformed file is an actionable failure, not a crash or a silent skip: the JSON in
`common/equipment.json` is unparsable, so the validator must report the file (and the line or
byte if the parser knows it) and exit non-zero, while the rest of the mods still load.