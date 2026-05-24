# Mask Blur Node

`Mask Blur` softens a `Mask` input and outputs a blurred `Mask`. It is useful after `Mask Path` when the path edge needs to blend more gently before feeding `Heightmap From Mask`, `Mask Blend`, or color generation.

## Inputs

| Pin | Type | Description |
| --- | --- | --- |
| Mask | Mask | Source mask to blur. |

## Outputs

| Pin | Type | Description |
| --- | --- | --- |
| Mask | Mask | Blurred mask. |

## Parameters

| Parameter | Description |
| --- | --- |
| Radius (m) | Blur radius in terrain meters. |
| Iterations | Number of blur passes. Higher values are smoother but more expensive. |
| Strength (%) | Mix amount between the original mask and blurred mask on each pass. |

## Notes

- The CPU implementation uses a separable horizontal/vertical blur.
- Each blur pass runs rows in parallel.
- `Radius (m)` is converted to pixels from the current terrain size and mask resolution.
