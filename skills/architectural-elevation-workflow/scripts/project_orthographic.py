"""Clip reviewed 3D edges against face loops and return four orthographic views."""

import numpy as np
from shapely.geometry import LineString, Polygon
from shapely.strtree import STRtree


VIEWS = {
    "front": ((0, 2, 1), (1, 1, -1)),
    "back": ((0, 2, 1), (-1, 1, 1)),
    "left": ((1, 2, 0), (-1, 1, -1)),
    "right": ((1, 2, 0), (1, 1, 1)),
}


def projected(points, view):
    coordinates = np.asarray(points, dtype=float)
    if coordinates.ndim != 2 or coordinates.shape[1] != 3 or not np.isfinite(coordinates).all():
        raise ValueError("Expected finite XYZ coordinates in millimetres")
    axes, signs = VIEWS[view]
    return coordinates[:, axes] * signs


def line_parts(geometry):
    if geometry.geom_type == "LineString":
        if not geometry.is_empty:
            yield geometry
    elif hasattr(geometry, "geoms"):
        for part in geometry.geoms:
            yield from line_parts(part)


def visible_intervals(hidden):
    cursor = 0.0
    for start, end in sorted(hidden):
        if start > cursor:
            yield cursor, start
        cursor = max(cursor, end)
    if cursor < 1.0:
        yield cursor, 1.0


def project_view(faces, edges, view, epsilon=1e-6, occlusion_records=None):
    """Input faces: loops_mm (outer loop then holes), optional occludes flag.

    Input edges: points_mm (two points), id/path and optional views. Select valid
    final-face edges and view-specific silhouettes before calling this function.
    Larger projected depth is nearer the viewer. Epsilon is numerical, in mm.
    """
    if not np.isfinite(epsilon) or epsilon <= 0:
        raise ValueError("epsilon must be positive and finite")
    masks, planes, mask_sources = [], [], []
    for face in faces:
        if not face.get("occludes", True):
            continue
        outer, *holes = [projected(loop, view) for loop in face["loops_mm"]]
        if len(outer) < 3:
            raise ValueError(f"Face {face.get('id')} has fewer than three points")
        origin = outer[0]
        all_points = np.concatenate([outer, *holes]) - origin
        gradients, _, rank, _ = np.linalg.lstsq(all_points[:, :2], all_points[:, 2], rcond=1e-12)
        if rank < 2:
            continue  # Edge-on faces have no area in this view.
        if np.max(np.abs(all_points[:, :2] @ gradients - all_points[:, 2])) > epsilon:
            raise ValueError(f"Face {face.get('id')} is not planar")
        mask = Polygon(outer[:, :2], [hole[:, :2] for hole in holes])
        if not mask.is_valid:
            raise ValueError(f"Face {face.get('id')} has invalid projected loops in {view}")
        masks.append(mask)
        planes.append((origin, gradients))
        mask_sources.append((face.get("id"), face.get("path", [])))

    index = STRtree(masks)
    output = []
    for edge in edges:
        if view not in edge.get("views", VIEWS):
            continue
        start, end = projected(edge["points_mm"], view)
        vector = end - start
        length_squared = vector[:2] @ vector[:2]
        if length_squared <= epsilon * epsilon:
            continue
        line = LineString([start[:2], end[:2]])
        hidden = []
        for i in index.query(line):
            origin, gradients = planes[i]
            delta = (start[:2] - origin[:2]) @ gradients + origin[2] - start[2]
            change = vector[:2] @ gradients - vector[2]
            if max(delta, delta + change) <= epsilon:
                continue
            for part in line_parts(line.intersection(masks[i])):
                u0, u1 = sorted(((np.asarray(p) - start[:2]) @ vector[:2]) / length_squared
                                for p in (part.coords[0], part.coords[-1]))
                if change > 0:
                    u0 = max(u0, (epsilon - delta) / change)
                elif change < 0:
                    u1 = min(u1, (epsilon - delta) / change)
                lo, hi = max(0.0, u0), min(1.0, u1)
                if hi > lo:
                    hidden.append((lo, hi))
                    if occlusion_records is not None:
                        face_id, face_path = mask_sources[i]
                        occlusion_records.append({"edge_id": edge.get("id"),
                                                  "edge_path": edge.get("path", []),
                                                  "face_id": face_id, "face_path": face_path,
                                                  "interval": [lo, hi]})
        for lo, hi in visible_intervals(hidden):
            output.append({**edge, "view": view, "interval": [lo, hi],
                           "points_2d_mm": [(start + vector * u)[:2].tolist() for u in (lo, hi)]})
    return output


def project_four(faces, edges, audit=None):
    """Project an already registered scene (X right, Y into building, Z up)."""
    faces, edges = list(faces), list(edges)
    return {view: project_view(faces, edges, view,
                               occlusion_records=None if audit is None else audit.setdefault(view, []))
            for view in VIEWS}
