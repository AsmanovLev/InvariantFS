# InvariantFS overview v2 — concept (approved direction, owner 2026-10-12)

Thesis: **complexity demands proof.** A plain FS barely touches bytes
(write blocks, read blocks) — trust is cheap there. InvariantFS runs
surgery on every byte (transcode → decompose → dedupe → re-encode →
migrate → seal): every transformation is a place to lie. At that
complexity, "trust me" is a lottery ticket. The invariant — prove every
step or refuse to hand over the bytes — is the engineering price of
aggressive storage, not a moral posture.

Structure (~75 s, hard cuts, scenes 3–8 s):
1. HOOK (0–3 s): flipping bit + stamp. ALMOST RIGHT IS WRONG.
2. TRUST vs PROOF (3–12 s): split screen. Left: plain FS, "trust the
   codec, trust the disk" (few boxes, thin arrows). Right: InvFS,
   every arrow carries a checkpoint stamp.
3. HERO decompose (12–28 s): qcow2/TAR container bursts into parts,
   each hashed, each living its own codec life; members on demand.
4. THE GAUNTLET (28–50 s): bytes run a conveyor through 6 transformer
   gates; each gate stamps PROVEN (round-trip memcmp) or the packet
   drops into a VERBATIM side tray. No stamp, no transform.
5. NUMBERS (50–62 s): stamp cards — 0.95× honest / 2.8× text / 3.76×
   dedup (all measured 2026-10-12, see prior notes).
6. SAVEPOINT + SEAL beats (62–70 s): flag plants, 2 s rewind; shield
   stamp 3 s. Seal is ONE beat, not a section.
7. CTA (70–76 s): install line types itself. PROVE IT, DON'T PROMISE IT.

Pacing rules (from research): ease everything, stagger entries ≤0.3 s,
one idea per scene, text ≥2 s on screen, phone-legible type (≥26 pt at
1080p), riser into every cut, ≤3 SFX per scene, pad bed throughout.
No voiceover: kinetic text IS the argument; must hold muted.
