# Offline clan events — owner request, October 27, 2026

The owner wants randomly arriving bot clanmates and entertaining clan activity
("causing chaos"). This is planned work, not implemented or verified behavior.
Continue the current arena progression/result task first. Advanced battle-AI
refinement remains deferred; social simulation is a separate feature.

Start with occasional joins/leaves, short authored conversations, friendly
challenges and legitimate card requests/donations. Use multiple persistent bot
identities, distinct names and finite wallets/card inventories. Joining must
respect the clan's capacity, join policy and trophy requirement. Existing kicked
bot tombstones must prevent the same bot silently rejoining without invitation.
Retain leader controls, role topology and player kick decisions. Clanmates may
be playful/rivalrous without changing player cards, spending player currency,
removing the player or taking leadership. Events must remain understandable in
the original clan stream; do not invent multiplayer connectivity.

Decide event timing using bounded persisted schedules/random state, not a roll
on every frame, page entry or login. Prevent login-spam events and unlimited
offline catch-up; handle clock rollback, long absence and process interruption.
Commit membership, event identity, bot economy and stream together before
delivering success. Repeated delivery must not duplicate joins or donations.
The present single Local Donor has real saved ownership, requests, roles,
weekly counters and kick/invite handling: extend that model systematically,
rather than adding UI-only fake roster entries or sharing one inventory among
new names. Preserve the seven original training missions throughout.

Qualification must cover deterministic clock/random fixtures; normal native
UI arrivals, messages, profile viewing, challenge/cancel, request/donation and
kick/invite; capacity/closed-clan cases; leader handoff; relaunch and interrupted
commit recovery; finite inventories and aggregate clan statistics. Separate
funded/component fixtures from earned progression. User-facing frequency and
variety can be tuned after the basic persisted lifecycle works.
