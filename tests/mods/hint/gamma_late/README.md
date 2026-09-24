Proves the load_after hint: this mod overrides `mod_hint_law` with cost 75.0. It sorts FIRST
by name, so without the hint it would load before `hint_early` and the override would land on
nothing; `load_after: ["hint_early"]` forces it after, and 75.0 is the winning value.