Proves dependency ordering: this mod sorts first by name and directory, yet it depends on
`zeta_base`, so `zeta_base` loads first and `alpha_top`'s definition of `mod_chain_law`
(cost 90.0) wins over the one `zeta_base` added (10.0).