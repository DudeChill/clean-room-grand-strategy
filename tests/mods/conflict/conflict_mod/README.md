Proves an append cannot silently win: `add_conscription_limited` tries to append a key that
already exists, which is a conflict reported as an error rather than a replacement, so the
validator exits non-zero. A plain `conscription_limited` key would have replaced it instead.