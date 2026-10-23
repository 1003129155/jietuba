//! 纯 Rust 几何工具：凸包、最小外接矩形、unclip 外扩、四边形透视裁剪。

pub type Pt = [f32; 2];

/// Andrew monotone chain 凸包，返回逆时针顺序的点。
pub fn convex_hull(mut pts: Vec<Pt>) -> Vec<Pt> {
    if pts.len() < 3 {
        return pts;
    }
    pts.sort_by(|a, b| {
        a[0].partial_cmp(&b[0]).unwrap().then(a[1].partial_cmp(&b[1]).unwrap())
    });
    pts.dedup();
    let n = pts.len();
    if n < 3 {
        return pts;
    }
    let cross = |o: Pt, a: Pt, b: Pt| -> f32 {
        (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    };
    let mut hull: Vec<Pt> = Vec::with_capacity(2 * n);
    // lower
    for &p in pts.iter() {
        while hull.len() >= 2 && cross(hull[hull.len() - 2], hull[hull.len() - 1], p) <= 0.0 {
            hull.pop();
        }
        hull.push(p);
    }
    // upper
    let lower_len = hull.len() + 1;
    for &p in pts.iter().rev() {
        while hull.len() >= lower_len && cross(hull[hull.len() - 2], hull[hull.len() - 1], p) <= 0.0 {
            hull.pop();
        }
        hull.push(p);
    }
    hull.pop();
    hull
}

/// 旋转卡壳求最小面积外接矩形，返回 4 个角点（顺时针，从左上开始大致）。
pub fn min_area_rect(hull: &[Pt]) -> Vec<Pt> {
    if hull.len() < 3 {
        // 退化：直接返回轴对齐包围盒
        return aabb(hull);
    }
    let n = hull.len();
    let mut best_area = f32::MAX;
    let mut best: Vec<Pt> = Vec::new();
    for i in 0..n {
        let p1 = hull[i];
        let p2 = hull[(i + 1) % n];
        let edge = [p2[0] - p1[0], p2[1] - p1[1]];
        let len = (edge[0] * edge[0] + edge[1] * edge[1]).sqrt();
        if len < 1e-6 {
            continue;
        }
        let ux = [edge[0] / len, edge[1] / len]; // 沿边方向
        let uy = [-ux[1], ux[0]]; // 法向
        let (mut min_x, mut max_x, mut min_y, mut max_y) = (f32::MAX, f32::MIN, f32::MAX, f32::MIN);
        for &p in hull {
            let dx = p[0] * ux[0] + p[1] * ux[1];
            let dy = p[0] * uy[0] + p[1] * uy[1];
            min_x = min_x.min(dx);
            max_x = max_x.max(dx);
            min_y = min_y.min(dy);
            max_y = max_y.max(dy);
        }
        let area = (max_x - min_x) * (max_y - min_y);
        if area < best_area {
            best_area = area;
            // 4 个角点（投影坐标系）→ 还原到世界坐标
            let corners = [
                [min_x, min_y],
                [max_x, min_y],
                [max_x, max_y],
                [min_x, max_y],
            ];
            best = corners
                .iter()
                .map(|c| [c[0] * ux[0] + c[1] * uy[0], c[0] * ux[1] + c[1] * uy[1]])
                .collect();
        }
    }
    if best.is_empty() {
        aabb(hull)
    } else {
        order_quad(best)
    }
}

fn aabb(pts: &[Pt]) -> Vec<Pt> {
    let (mut minx, mut miny, mut maxx, mut maxy) = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);
    for &p in pts {
        minx = minx.min(p[0]);
        miny = miny.min(p[1]);
        maxx = maxx.max(p[0]);
        maxy = maxy.max(p[1]);
    }
    vec![[minx, miny], [maxx, miny], [maxx, maxy], [minx, maxy]]
}

/// 把矩形的 4 个角排成 左上、右上、右下、左下：按 x 分出左右两对，每对里靠上的在前。
/// 斜着的文字行因此仍从左上角读到右上角。不按 x+y、x−y 取角：框斜 45° 时它们两两相等，会取出重复的角。
pub fn order_quad(pts: Vec<Pt>) -> Vec<Pt> {
    let mut p = pts;
    if p.len() != 4 {
        return p;
    }
    p.sort_by(|a, b| a[0].total_cmp(&b[0]));
    let (tl, bl) = if p[0][1] <= p[1][1] { (p[0], p[1]) } else { (p[1], p[0]) };
    let (tr, br) = if p[2][1] <= p[3][1] { (p[2], p[3]) } else { (p[3], p[2]) };
    vec![tl, tr, br, bl]
}

/// 点是否在凸多边形内（含边界），顶点顺时针、逆时针均可。
pub fn convex_contains(poly: &[Pt], p: Pt) -> bool {
    let mut side = 0.0f32;
    for (i, &a) in poly.iter().enumerate() {
        let b = poly[(i + 1) % poly.len()];
        let cross = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
        if cross != 0.0 {
            if side != 0.0 && (cross > 0.0) != (side > 0.0) {
                return false;
            }
            side = cross;
        }
    }
    true
}

/// 多边形面积（鞋带公式，绝对值）。
pub fn poly_area(pts: &[Pt]) -> f32 {
    let n = pts.len();
    let mut s = 0.0;
    for i in 0..n {
        let j = (i + 1) % n;
        s += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1];
    }
    s.abs() / 2.0
}

/// 多边形周长。
pub fn poly_perimeter(pts: &[Pt]) -> f32 {
    let n = pts.len();
    let mut s = 0.0;
    for i in 0..n {
        let j = (i + 1) % n;
        let dx = pts[j][0] - pts[i][0];
        let dy = pts[j][1] - pts[i][1];
        s += (dx * dx + dy * dy).sqrt();
    }
    s
}

/// unclip：把四边形按 distance = area*ratio/perimeter 向外扩张。
/// 正确实现：把有序四边形(TL,TR,BR,BL)当作旋转矩形，沿其自身的宽/高轴
/// 对每条边垂直外扩 distance（等价于 Clipper 对矩形的多边形偏移）。
/// 旧的"径向外扩"对细长框高度方向扩得不足，会把文字上下切掉。
pub fn unclip(quad: &[Pt], ratio: f32) -> Vec<Pt> {
    if quad.len() != 4 {
        return quad.to_vec();
    }
    let area = poly_area(quad);
    let peri = poly_perimeter(quad);
    if peri < 1e-6 {
        return quad.to_vec();
    }
    let dist = area * ratio / peri;

    let tl = quad[0];
    let tr = quad[1];
    let br = quad[2];
    let bl = quad[3];

    // 宽方向单位向量 u (TL->TR)，高方向单位向量 v (TL->BL)
    let mut u = [tr[0] - tl[0], tr[1] - tl[1]];
    let mut v = [bl[0] - tl[0], bl[1] - tl[1]];
    let lu = (u[0] * u[0] + u[1] * u[1]).sqrt().max(1e-6);
    let lv = (v[0] * v[0] + v[1] * v[1]).sqrt().max(1e-6);
    u = [u[0] / lu, u[1] / lu];
    v = [v[0] / lv, v[1] / lv];

    let du = [u[0] * dist, u[1] * dist];
    let dv = [v[0] * dist, v[1] * dist];

    vec![
        [tl[0] - du[0] - dv[0], tl[1] - du[1] - dv[1]], // TL: -u -v
        [tr[0] + du[0] - dv[0], tr[1] + du[1] - dv[1]], // TR: +u -v
        [br[0] + du[0] + dv[0], br[1] + du[1] + dv[1]], // BR: +u +v
        [bl[0] - du[0] + dv[0], bl[1] - du[1] + dv[1]], // BL: -u +v
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn corners_run_clockwise_from_the_top_left_of_the_line() {
        let level = order_quad(vec![[10.0, 3.0], [0.0, 0.0], [0.0, 3.0], [10.0, 0.0]]);
        assert_eq!(level, vec![[0.0, 0.0], [10.0, 0.0], [10.0, 3.0], [0.0, 3.0]]);
        // 往右上斜 45°：长边从 (0,4) 到 (4,0)
        let diagonal = order_quad(vec![[4.0, 0.0], [5.0, 1.0], [1.0, 5.0], [0.0, 4.0]]);
        assert_eq!(diagonal, vec![[0.0, 4.0], [4.0, 0.0], [5.0, 1.0], [1.0, 5.0]]);
        // 往右上斜约 63°：长边 (0,10)→(4,2) 仍在上方，不当成竖排
        let steep = order_quad(vec![[6.0, 3.0], [2.0, 11.0], [0.0, 10.0], [4.0, 2.0]]);
        assert_eq!(steep, vec![[0.0, 10.0], [4.0, 2.0], [6.0, 3.0], [2.0, 11.0]]);
    }

    /// xorshift64*：测试只要可复现的伪随机，不引入新依赖。
    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 >> 12;
            self.0 ^= self.0 << 25;
            self.0 ^= self.0 >> 27;
            self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
        }
        fn range(&mut self, lo: i32, hi: i32) -> i32 {
            lo + (self.next() % (hi - lo + 1) as u64) as i32
        }
        fn unit(&mut self) -> f32 {
            (self.next() >> 40) as f32 / (1u64 << 24) as f32
        }
    }

    type IPoint = imageproc::point::Point<i32>;

    fn to_ipoints(pts: &[Pt]) -> Vec<IPoint> {
        pts.iter().map(|p| IPoint::new(p[0] as i32, p[1] as i32)).collect()
    }

    fn sorted(mut pts: Vec<(i32, i32)>) -> Vec<(i32, i32)> {
        pts.sort();
        pts
    }

    /// 整数点云：随机散点，混入重复点和落在同一条线上的点。
    fn point_cloud(rng: &mut Rng) -> Vec<Pt> {
        let n = rng.range(3, 40);
        let mut pts: Vec<Pt> = (0..n).map(|_| [rng.range(-50, 50) as f32, rng.range(-50, 50) as f32]).collect();
        let a = pts[0];
        for k in 1..=rng.range(0, 4) {
            pts.push([a[0] + 3.0 * k as f32, a[1] - 2.0 * k as f32]);
        }
        pts.push(pts[1]);
        pts
    }

    /// 旋转矩形里的整数点：四个角加内部随机点，角度 0–180°，含 45°、90° 这类特殊角。
    fn rotated_cloud(rng: &mut Rng, i: usize) -> (Vec<Pt>, f32, f32, f32) {
        let degrees = match i % 6 {
            0 => 45.0,
            1 => 90.0,
            2 => 0.0,
            _ => rng.unit() * 180.0,
        };
        let (w, h) = (rng.range(20, 300) as f32, rng.range(5, 60) as f32);
        let (s, c) = degrees.to_radians().sin_cos();
        let (cx, cy) = (rng.range(200, 800) as f32, rng.range(200, 800) as f32);
        let at = |u: f32, v: f32| -> Pt { [(cx + u * c - v * s).round(), (cy + u * s + v * c).round()] };
        let mut pts = vec![at(-w / 2.0, -h / 2.0), at(w / 2.0, -h / 2.0), at(w / 2.0, h / 2.0), at(-w / 2.0, h / 2.0)];
        for _ in 0..rng.range(0, 60) {
            pts.push(at((rng.unit() - 0.5) * w, (rng.unit() - 0.5) * h));
        }
        (pts, degrees, w, h)
    }

    #[test]
    fn convex_hull_matches_imageproc() {
        let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
        for case in 0..2000 {
            let pts = point_cloud(&mut rng);
            let hull = convex_hull(pts.clone());
            let mut unique = to_ipoints(&pts);
            unique.sort_by_key(|p| (p.x, p.y));
            unique.dedup();
            let expected = sorted(imageproc::geometry::convex_hull(unique).iter().map(|p| (p.x, p.y)).collect());
            let got = sorted(hull.iter().map(|p| (p[0] as i32, p[1] as i32)).collect());
            assert_eq!(got, expected, "case {case}: {pts:?}");
            // 相邻三点同向转（y 向上时逆时针），中途不折返
            if hull.len() >= 3 {
                for k in 0..hull.len() {
                    let (o, a, b) = (hull[k], hull[(k + 1) % hull.len()], hull[(k + 2) % hull.len()]);
                    let cross = (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
                    assert!(cross > 0.0, "case {case}: {hull:?}");
                }
            }
        }
    }

    #[test]
    fn min_area_rect_matches_imageproc() {
        let mut rng = Rng(0x2545_f491_4f6c_dd1d);
        for case in 0..2000 {
            let (pts, degrees, w, h) = rotated_cloud(&mut rng, case);
            let rect = min_area_rect(&convex_hull(pts.clone()));
            assert_eq!(rect.len(), 4);
            // 点都在框内（容许浮点误差）
            for &p in &pts {
                let inside = (0..4).all(|k| {
                    let (a, b) = (rect[k], rect[(k + 1) % 4]);
                    let len = dist2(a, b).sqrt().max(1e-6);
                    ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / len >= -0.01
                });
                assert!(inside, "case {case}: {p:?} outside {rect:?}");
            }
            // 面积与 imageproc 一致：它把角点向外取整，每条边最多外移约 1px
            let reference = imageproc::geometry::min_area_rect(&to_ipoints(&pts));
            let reference: Vec<Pt> = reference.iter().map(|p| [p.x as f32, p.y as f32]).collect();
            let slack = poly_perimeter(&reference) * 1.5 + 4.0;
            let (area, expected) = (poly_area(&rect), poly_area(&reference));
            assert!((area - expected).abs() <= slack, "case {case}: area {area} vs imageproc {expected}, {degrees}°");
            // 细长框的长边方向就是生成时的角度（模 180°）
            if w >= 2.0 * h && w >= 40.0 {
                let long = if dist2(rect[0], rect[1]) >= dist2(rect[1], rect[2]) { (rect[0], rect[1]) } else { (rect[1], rect[2]) };
                let angle = (long.1[1] - long.0[1]).atan2(long.1[0] - long.0[0]).to_degrees().rem_euclid(180.0);
                let diff = (angle - degrees).rem_euclid(180.0);
                assert!(diff.min(180.0 - diff) < 3.0, "case {case}: long edge at {angle}°, drawn at {degrees}°");
            }
        }
    }

    /// 散点的最小外接矩形不必和某个生成框对齐：按 0.1° 一档转一圈求外接框面积，最优解不会更大。
    #[test]
    fn min_area_rect_is_no_larger_than_any_rotation() {
        let mut rng = Rng(0x1234_5678_9abc_def1);
        for case in 0..500 {
            let pts = point_cloud(&mut rng);
            let hull = convex_hull(pts.clone());
            if hull.len() < 3 {
                continue;
            }
            let area = poly_area(&min_area_rect(&hull));
            let best = (0..900)
                .map(|step| {
                    let (s, c) = (step as f32 * 0.1).to_radians().sin_cos();
                    let (mut lo_u, mut hi_u, mut lo_v, mut hi_v) = (f32::MAX, f32::MIN, f32::MAX, f32::MIN);
                    for p in &pts {
                        let (u, v) = (p[0] * c + p[1] * s, p[1] * c - p[0] * s);
                        lo_u = lo_u.min(u);
                        hi_u = hi_u.max(u);
                        lo_v = lo_v.min(v);
                        hi_v = hi_v.max(v);
                    }
                    (hi_u - lo_u) * (hi_v - lo_v)
                })
                .fold(f32::MAX, f32::min);
            assert!(area <= best * 1.0001 + 0.01, "case {case}: {area} > {best} for {pts:?}");
            assert!(area >= poly_area(&hull) - 0.01, "case {case}");
        }
    }

    fn dist2(a: Pt, b: Pt) -> f32 {
        (a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)
    }

    /// 旋转 0–359° 的长方形：结果从左上角起顺时针（y 向下）。斜得不超过 tan⁻¹(长/宽) 时
    /// 左上→右上是长边；更陡就当竖排，左上→右上是短边。
    #[test]
    fn quads_start_top_left_and_run_clockwise_at_every_angle() {
        for (w, h) in [(40.0f32, 10.0f32), (15.0, 10.0)] {
            let limit = (w / h).atan().to_degrees();
            for degrees in 0..360 {
                // 往右上斜为正：沿行方向 (cos, −sin)，行的下方 (sin, cos)
                let (s, c) = (degrees as f32).to_radians().sin_cos();
                let a: Pt = [100.0, 100.0];
                let b: Pt = [a[0] + w * c, a[1] - w * s];
                let d: Pt = [a[0] + h * s, a[1] + h * c];
                let e: Pt = [b[0] + h * s, b[1] + h * c];
                let corners = [a, b, e, d];
                let quad = order_quad(vec![e, a, d, b]);
                let close = |p: Pt, q: Pt| dist2(p, q) < 1e-6;
                let shift = (0..4).find(|&k| close(quad[0], corners[k])).unwrap_or_else(|| panic!("{degrees}°: {quad:?}"));
                for k in 0..4 {
                    assert!(close(quad[k], corners[(shift + k) % 4]), "{w}x{h} {degrees}°: not clockwise {quad:?}");
                }
                let mut by_x = corners.to_vec();
                by_x.sort_by(|p, q| p[0].total_cmp(&q[0]));
                let top_left = if by_x[0][1] <= by_x[1][1] { by_x[0] } else { by_x[1] };
                assert!(close(quad[0], top_left), "{w}x{h} {degrees}°: starts at {:?}", quad[0]);
                // 行方向折到 (−90°, 90°]
                let tilt = ((degrees as f32 + 90.0).rem_euclid(180.0) - 90.0).abs();
                if (tilt - limit).abs() < 1.0 {
                    continue;
                }
                let first = dist2(quad[0], quad[1]).sqrt();
                let want = if tilt < limit { w } else { h };
                assert!((first - want).abs() < 1e-3, "{w}x{h} {degrees}°: first edge {first}, want {want}");
            }
        }
    }

    #[test]
    fn containment_includes_the_edges_of_a_tilted_box() {
        let quad = [[0.0, 4.0], [4.0, 0.0], [5.0, 1.0], [1.0, 5.0]];
        assert!(convex_contains(&quad, [2.5, 2.5]));
        assert!(convex_contains(&quad, [2.0, 2.0]));
        assert!(convex_contains(&quad, [4.0, 0.0]));
        assert!(!convex_contains(&quad, [0.0, 0.0]));
        assert!(!convex_contains(&quad, [5.0, 5.0]));
        let reversed: Vec<Pt> = quad.iter().rev().copied().collect();
        assert!(convex_contains(&reversed, [2.5, 2.5]));
        assert!(!convex_contains(&reversed, [0.0, 0.0]));
    }
}
