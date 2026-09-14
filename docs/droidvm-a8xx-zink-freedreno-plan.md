# DroidVM A8xx Zink/Freedreno optimization plan

## Current architecture

The current Mesa path is expected to remain:

OpenGL application

-> Zink Gallium driver

-> Vulkan

-> Turnip/Freedreno Vulkan backend

This work does not attempt to create a new Zink-to-Freedreno direct translator.
The goal is to remove overhead from the existing path.

## Phase 1: measure before changing behavior

Collect:

- Zink batch submit frequency
- pipeline cache hit rate
- descriptor update frequency
- NIR lowering cost
- Turnip command buffer submission cost

No workload result is accepted without matching shader cache and driver revision.

## Phase 2: Zink hot paths

Candidate areas:

- batch reuse
- descriptor pool lifetime
- pipeline creation cache
- unnecessary state emission
- NIR pass duplication

## Phase 3: Turnip/Freedreno hot paths

Candidate areas:

- A8xx shader variant cache
- descriptor upload reuse
- command stream batching
- resource state transition reduction

## Acceptance

Each optimization requires:

1. Mesa build validation.
2. Vulkan/OpenGL regression tests.
3. DroidVM workload comparison.
4. No regression on existing WDDM path.

This document tracks engineering work. It does not claim an OpenGL direct migration exists.
