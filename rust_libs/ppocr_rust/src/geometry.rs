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

/// 把 4 个点排成 左上、右上、右下、左下。
pub fn order_quad(pts: Vec<Pt>) -> Vec<Pt> {
    let mut p = pts;
    // 按 (x+y) 找左上/右下，按 (x-y) 找右上/左下
    let tl = *p.iter().min_by(|a, b| (a[0] + a[1]).partial_cmp(&(b[0] + b[1])).unwrap()).unwrap();
    let br = *p.iter().max_by(|a, b| (a[0] + a[1]).partial_cmp(&(b[0] + b[1])).unwrap()).unwrap();
    let tr = *p.iter().max_by(|a, b| (a[0] - a[1]).partial_cmp(&(b[0] - b[1])).unwrap()).unwrap();
    let bl = *p.iter().min_by(|a, b| (a[0] - a[1]).partial_cmp(&(b[0] - b[1])).unwrap()).unwrap();
    p = vec![tl, tr, br, bl];
    p
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
