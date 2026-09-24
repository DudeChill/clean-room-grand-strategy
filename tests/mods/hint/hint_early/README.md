Part of the load_after pair: adds the law `mod_hint_law` (cost 20.0). It sorts last by name,
but `gamma_late` declares `load_after: ["hint_early"]`, so this mod loads first.