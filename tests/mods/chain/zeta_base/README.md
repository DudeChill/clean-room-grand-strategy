Part of the dependency chain: this mod adds the new law `mod_chain_law` (cost 10.0). It is
named `zeta_base` and sits in directory `zeta_base`, so the name/directory tie-break would
load it AFTER `alpha_top`; only the dependency declared by `alpha_top` puts it first.