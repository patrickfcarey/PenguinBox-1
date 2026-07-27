# Runbooks — step-by-step procedures

Same convention as the sibling forks: a runbook is a **procedure you follow at
the machine**, written so it can be executed without re-deriving anything —
distinct from the plan (design + history, [docs/vr/00-mvp-plan.md](../docs/vr/00-mvp-plan.md)),
the methodology ([RESEARCH.md](../RESEARCH.md)), and the action queue
([TEST_QUEUE.md](../TEST_QUEUE.md)).

| Runbook | When |
|---|---|
| [rig-testing.md](rig-testing.md) | Any rig session — flat verification or in-headset VR testing: pre-flight, launch, expected log sequence, failure table, teardown order, post-test capture |

**Conventions for adding one:** procedures only (numbered steps, exact
commands, expected output, failure→meaning tables); name the hard rules it
enforces (H-numbers); keep secrets/PII out — this repo is public (H-8). Capture
protocols for the RAM hunt live in [RESEARCH.md](../RESEARCH.md) §2–§3 and are
not duplicated here.
