# Omega Core 0.20.6

Omega Core 0.20.6 is a mandatory network upgrade release.

## Summary

- Bumps the advertised network protocol to `70232`.
- Rejects peers below protocol `70232`.
- Requires masternodes participating in DKGs to use protocol `70232`.

## Upgrade rationale

Mainnet crossed the LWMA transition at block `3200000`, and the corrective LWMA
boundary intended for block `3200070` must be enforced by a distinct release.
Nodes remaining on the previous `70231` release line can still appear protocol
compatible while validating the old retarget behaviour.

This release creates a clean compatibility boundary for the corrected rule set.

## Operator action

- Upgrade all trusted nodes and miners to Omega Core `0.20.6`.
- Disconnect or ban peers that remain on the old release line.
- Do not mix `70231` and `70232` nodes during rollout if you want to stay on the
  corrected chain.
