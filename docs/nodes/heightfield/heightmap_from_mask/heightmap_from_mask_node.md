# Heightmap From Mask Node

`Heightmap From Mask` converts a `Mask` input into a `HeightField` output. It is intended for workflows such as drawing a path mask and then turning that mask into raised or carved terrain.

## Inputs

| Pin | Type | Description |
| --- | --- | --- |
| Mask | Mask | Source mask. Values near 0 become base height, and values near 1 become full height. |

## Outputs

| Pin | Type | Description |
| --- | --- | --- |
| Heightmap | HeightField | Generated heightfield. |

## Parameters

| Parameter | Description |
| --- | --- |
| Height (m) | Height added when the mask value is 1. Negative values carve downward. |
| Base Height (m) | Height used when the mask value is 0. |
| Gamma | Curve applied to the mask before height conversion. Higher values make low mask values flatter. |
| Invert | Inverts the mask before conversion. |

## Notes

- The node resamples the input mask to the simulation resolution before converting it.
- `Height = 100` and `Base Height = 0` maps mask value `0..1` to `0..100m`.
- Path point `Width`, `Feather`, and `Intensity` affect the mask upstream, so they are reflected in this node through the mask input.
