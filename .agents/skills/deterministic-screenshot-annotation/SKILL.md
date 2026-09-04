---
name: deterministic-screenshot-annotation
description: Deterministically annotate screenshots with ROIs, bounding boxes, click points, coordinates, masks, or diagnostic labels. Use for requests to draw MAA recognition regions or input locations on screenshots; generative image editing is unsuitable because the underlying pixels and geometry must remain exact.
---

# Deterministic Screenshot Annotation

Treat the screenshot as diagnostic evidence. Preserve every source pixel outside the requested overlays and keep the original dimensions.

## Workflow

1. Read the source image dimensions and identify the coordinate space used by every ROI or point.
2. Convert coordinates explicitly when the log and screenshot use different resolutions. MAA task coordinates normally use a `1280 × 720` reference frame; for a screenshot of size `W × H`, map `(x, y, w, h)` to `(xW/1280, yH/720, wW/1280, hH/720)` unless the evidence specifies another transform.
3. Draw overlays with a deterministic raster tool such as Pillow, OpenCV, or ImageMagick:
   - rectangles for ROIs and recognition boxes;
   - crosshairs or small circles centered on exact click coordinates;
   - leader lines and compact labels that do not obscure the target;
   - a legend when two or more overlay types are present.
4. Save to a new file beside the source or under a task-specific diagnostics directory. Never overwrite the evidence image unless the user explicitly requests replacement.
5. Verify the output dimensions, every transformed coordinate, and that unchanged regions are pixel-identical to the source before presenting the result.

## Hard guardrail

Use deterministic compositing for coordinate-sensitive annotation. Do not use image generation or generative image editing for ROI, click-position, bounding-box, mask, or pixel-comparison diagrams because it may move, redraw, crop, rescale, or invent interface content.

When coordinates overlap, preserve their exact centers and distinguish them with labels such as `×2` or separate leader lines rather than moving the markers for readability.
