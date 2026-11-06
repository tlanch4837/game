# Human ragdoll

Code: `src/ragdoll.h` (API, no Jolt types), `src/ragdoll.cpp` (Jolt side), `src/ragdoll_actor.h` (glue to the animator and scene
entities), `PhysicsWorld::Ragdolls()` in `physics.h`. Test: `ragdoll_test.bat` (headless, real Wyatt skeleton).

Sources of ideas: the Endorphin numbers in OpenDMS `anatomical_ragdoll_blueprint.md` (collision matrix, asymmetric limits, force-based
motors), the layered-ragdoll video (soft tension, animation layered under the physics, protective pose, dislocation by moving the
joint range into the hyper-extended zone, per-limb blast impulses), and published segment data (Winter/Dempster masses, joint
ranges, peak joint torques). The Endorphin installer in Downloads was not run; only its documented behaviour was used.

## Body

17 rigid bodies, one per joint frame of the skeleton (body rotation = skin joint rotation, so animation rotations are direct motor
targets): pelvis, abdomen (spine_01..03), chest (spine_03, clavicles ride on it), neck, head, upper arm / forearm / hand x2,
thigh / calf / foot x2. Masses are fractions of `RagdollTuning::massKg` (Winter: trunk 49.7 %, thigh 10 %, shank 4.65 %, foot 1.45 %,
upper arm 2.8 %, forearm 1.6 %, hand 0.6 %, head 6.7 %, neck 1.4 %). Capsules along the bones, boxes for pelvis/torso/feet, a sphere for
the head. Sizes scale with leg length.

Every other skin joint rides rigidly on its nearest simulated ancestor (fingers on the hand, toes on the foot). `spine_02` is shared
halfway between abdomen and chest so the spine bends smoothly in the skin.

Collisions: a Jolt group filter turns off only neighbouring pairs. Legs, arms and torso collide with each other (limbs stack on the
floor instead of passing through). Bodies use linear-cast motion quality, 12 velocity / 4 position solver steps, and the world
steps at 120 Hz or faster while any ragdoll exists (`RagdollWorld::Substeps`).

## Joints (anatomical)

| Joint | Type | Range |
|---|---|---|
| knee, elbow | hinge | -0.04 (-0.06) .. 2.45 (2.50) rad, hyper-extension stop |
| hip | swing-twist | flexion 2.0, extension 0.35, ab/adduction 0.75, twist +-0.65 |
| shoulder | swing-twist | flexion 3.15, extension 1.05, lateral 1.45, twist +-1.2 |
| spine (2), neck, head | swing-twist | biased cones, twist +-0.3..0.9 |
| ankle, wrist | swing-twist | plantar 0.8 / dorsi 0.35; wrist +-0.85 |

A cone's centre is tilted toward the flexion direction (`bias`), so a hip bends far forward and little back (Endorphin's
`PrimaryFrameOffset`). Constraints are made in the bind pose; bodies are then moved to the start pose, so the character can turn into a
ragdoll from any animation frame.

## Muscles

Each joint has a spring-damper motor aiming at the animation's local rotation (`SetTargetOrientationBS` / hinge target angle), with a
torque ceiling from strength tables: knee 220, hip 260, lumbar 200, chest 220, shoulder 110, elbow 75, ankle 100, neck 55 N m.
`tension` 0..1 scales frequency and (quadratically) torque; below 3 % the motor switches off and joint friction takes over (limp).
Bracing adds up to 35 % to the muscle drive.

The pose the muscles aim at is the animator's own (the clips keep running under the override): `SkeletalAnimator::AnimatedPose`.
`PlayerCharacter` plays the jump clip as the falling animation, faster while moving up and slower coming down.

## Reflexes

* **Brace** (`RagdollStatus::brace`): builds after `reactionTime` in the air when the pelvis falls faster than `braceSpeed` within
  about 3 m of the ground, or when tumbling fast near it. Holds through the impact, fades when still. The pose: arms raised in front of
  the face and across, elbows bent 2.1 rad, hips flexed 1.3, knees bent 1.9, chin and spine tucked.
* **Reach**: the landing spot is a ray along the velocity; an arm that can reach it (within about 0.6 m of the shoulder) points at it
  and straightens, so hands go out to catch the fall. Not a full hand-stepping system yet.
* **Unconscious**: muscles fade to nothing in about a third of a second.

## Injuries (`RagdollTuning::injuries`, `toughness` scales all)

* **Dislocation**: the load through a joint stop (constraint impulse / step time, N m) above a per-joint tolerance (elbow 90, shoulder
  160, knee 260, hip 380, ankle 700 ...; muscles raise it up to 30 %). Hinges only count the hyper-extension side. The joint's range
  moves into the hyper-extended zone and stays there (Jolt needs min <= 0 <= max, so the hinge is rebuilt with its zero turned and
  `hingeRef` offsets the motor targets); ball joints open by +0.75 / +0.55 rad and the twist range widens. Both segments get a jolt;
  the joint keeps 30 % strength.
* **Fracture**: a hit on a long bone above its impulse tolerance (forearm 38, shin 75, thigh 130 N s ...) leaves that limb at 12 %
  strength and weakens the joint above. There is no visible mid-bone bend yet: the skin has one joint per bone.
* **Knockout**: head impact above 60 N s or trunk above 290 N s.
* The impulse is `reduced mass x closing speed x effMass`, where `effMass` accounts for the body behind a limb (foot 3.0, hand 3.5).

## Forces

`Blast(centre, speed, radius)`: quadratic falloff, lifts as well as pushes, distal limbs (forearms, hands, shins, feet) get up to 1.5x
and their own sideways and spin components, so arms and legs do not move together. `Hit(point, impulse)` goes to the nearest part.

## Events

`TakeEvents()` returns impacts (part, closing speed, impulse, point; `other == 1` when two parts of the same body touch), dislocations,
fractures and knockouts. `PlayerCharacter` plays the footstep sound at a low pitch for impacts for now; the procedural sound system
(sliding, per-part material) is next.

## Use in Play

`R` ragdoll / stand up, `B` blast behind him, `K` knockout / wake, `H` punch to the head. `TBD_PLAYTEST` scripts take `ragdoll`,
`blast`, `knockout`, `dislocate`, `fracture`, `punch`. In code: `PlayerCharacter::Request(n)`, `ragdollTuning`.

## Not done

* Self-balancing / walking as a ragdoll, and getting up with an animation (standing up is an instant teleport to a capsule).
* Hands that step and roll the fall, feet that catch (needs a procedural gait).
* Visible bone breaks; face/jaw/eyes/hair/cloth physics.
* Sound beyond the stand-in impact thud.
* Vulkan/D3D12: no renderer change was needed (the skin goes through the animator on both).
