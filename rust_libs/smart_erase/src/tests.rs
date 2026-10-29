use super::*;

struct Picture {
    width: usize,
    height: usize,
    rgba: Vec<u8>,
}

impl Picture {
    fn new(width: usize, height: usize, mut color: impl FnMut(usize, usize) -> [u8; 4]) -> Self {
        let mut rgba = Vec::with_capacity(width * height * 4);
        for y in 0..height {
            for x in 0..width {
                rgba.extend_from_slice(&color(x, y));
            }
        }
        Self { width, height, rgba }
    }

    fn image(&self, order: ChannelOrder) -> Image<'_> {
        Image { pixels: &self.rgba, width: self.width, height: self.height, stride: self.width * 4, order }
    }
}

fn rect_mask(width: usize, height: usize) -> Vec<u8> {
    vec![1; width * height]
}

/// Erases the rectangle (x, y, w, h) and returns the solver with the rectangle's RGBA bytes.
fn erase_rect(picture: &Picture, x: usize, y: usize, w: usize, h: usize) -> (Solver, Vec<u8>) {
    let mask = rect_mask(w, h);
    let mut out = vec![0; w * h * 4];
    let solver = erase(&picture.image(ChannelOrder::Rgba), &Hole { x, y, width: w, height: h, mask: &mask }, &|| false, &mut out).unwrap();
    (solver, out)
}

fn pixel(out: &[u8], w: usize, x: usize, y: usize) -> [u8; 4] {
    let at = (y * w + x) * 4;
    [out[at], out[at + 1], out[at + 2], out[at + 3]]
}

fn noise(seed: u32) -> impl FnMut() -> u32 {
    let mut state = seed;
    move || {
        state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        state >> 8
    }
}

#[test]
fn a_chat_bubble_on_a_flat_background_disappears_without_a_trace() {
    let picture = Picture::new(200, 120, |x, y| {
        let bubble = (60..140).contains(&x) && (40..70).contains(&y);
        let glyph = bubble && (x + y) % 5 == 0;
        match (bubble, glyph) {
            (_, true) => [25, 25, 25, 255],
            (true, false) => [0x95, 0xEC, 0x69, 255],
            _ => [0xF5, 0xF5, 0xF5, 255],
        }
    });
    let (solver, out) = erase_rect(&picture, 54, 34, 92, 42);
    assert_eq!(solver, Solver::Surface);
    for y in 0..42 {
        for x in 0..92 {
            assert_eq!(pixel(&out, 92, x, y), [0xF5, 0xF5, 0xF5, 255], "at {x},{y}");
        }
    }
}

#[test]
fn content_just_beside_the_hole_does_not_spoil_a_flat_fill() {
    // The square ends two pixels above the hole, inside the wide ring but outside the narrow one.
    let picture = Picture::new(160, 120, |x, y| {
        if (60..90).contains(&x) && (40..60).contains(&y) {
            [220, 30, 30, 255]
        } else {
            [128, 128, 128, 255]
        }
    });
    let (solver, out) = erase_rect(&picture, 52, 62, 46, 36);
    assert_eq!(solver, Solver::Surface);
    assert!(out.chunks(4).all(|p| p == [128, 128, 128, 255]));
}

#[test]
fn an_antialiased_fringe_left_around_the_hole_is_tolerated() {
    // Glyph edges one pixel above the hole are too many outliers for the narrow ring (about
    // 4%) but few enough for the wide one (about 2%).
    let picture = Picture::new(120, 80, |x, y| {
        let fringe = y == 29 && (30..90).contains(&x) && x % 4 == 0;
        if fringe {
            [170, 170, 170, 255]
        } else {
            [245, 245, 245, 255]
        }
    });
    let (solver, out) = erase_rect(&picture, 30, 30, 60, 21);
    assert_eq!(solver, Solver::Surface);
    assert!(out.chunks(4).all(|p| p == [245, 245, 245, 255]));
}

#[test]
fn a_linear_gradient_continues_through_the_hole() {
    let picture = Picture::new(256, 200, |x, y| [x as u8, y as u8, 100, 255]);
    let (solver, out) = erase_rect(&picture, 90, 70, 80, 50);
    assert_eq!(solver, Solver::Surface);
    for y in 0..50 {
        for x in 0..80 {
            let p = pixel(&out, 80, x, y);
            assert!(p[0].abs_diff((90 + x) as u8) <= 1 && p[1].abs_diff((70 + y) as u8) <= 1 && p[2].abs_diff(100) <= 1, "{p:?} at {x},{y}");
        }
    }
}

#[test]
fn a_checkerboard_is_copied_along_its_period() {
    let cell = |x: usize, y: usize| if (x / 8 + y / 8) % 2 == 0 { 255 } else { 227 };
    let picture = Picture::new(240, 160, |x, y| {
        let v = cell(x, y);
        [v, v, v, 255]
    });
    let (solver, out) = erase_rect(&picture, 100, 60, 40, 24);
    assert_eq!(solver, Solver::Periodic);
    for y in 0..24 {
        for x in 0..40 {
            let v = cell(100 + x, 60 + y);
            assert_eq!(pixel(&out, 40, x, y), [v, v, v, 255], "at {x},{y}");
        }
    }
}

#[test]
fn an_object_cut_by_the_hole_is_not_repeated_across_it() {
    // A flat background matches any period; the square on the right must not be copied left.
    let square = |x: usize, y: usize| (150..190).contains(&x) && (30..70).contains(&y);
    let picture = Picture::new(220, 100, |x, y| if square(x, y) { [0xE0, 0xA8, 0x6F, 255] } else { [0xF5, 0xF5, 0xF5, 255] });
    let (solver, out) = erase_rect(&picture, 40, 25, 130, 50);
    assert_ne!(solver, Solver::Periodic);
    for y in 0..50 {
        for x in 0..90 {
            let p = pixel(&out, 130, x, y);
            assert!(p[..3].iter().all(|v| v.abs_diff(0xF5) <= 4), "{p:?} at {x},{y}");
        }
    }
}

#[test]
fn a_smooth_curved_background_is_bridged_by_the_membrane() {
    // x^2 - y^2 is harmonic, so the membrane reproduces it while no plane fits the ring.
    let saddle = |x: usize, y: usize| {
        let (dx, dy) = (x as f32 - 150.0, y as f32 - 100.0);
        0.004 * (dx * dx - dy * dy)
    };
    let picture = Picture::new(300, 200, |x, y| {
        let s = saddle(x, y);
        [(120.0 + s).round() as u8, (100.0 + 0.5 * s).round() as u8, 160, 255]
    });
    let (solver, out) = erase_rect(&picture, 100, 60, 100, 80);
    assert_eq!(solver, Solver::Membrane);
    for y in 0..80 {
        for x in 0..100 {
            let s = saddle(100 + x, 60 + y);
            let p = pixel(&out, 100, x, y);
            assert!((f32::from(p[0]) - 120.0 - s).abs() <= 3.0 && (f32::from(p[1]) - 100.0 - 0.5 * s).abs() <= 3.0, "{p:?} at {x},{y}");
        }
    }
}

#[test]
fn a_noisy_texture_is_synthesized_rather_than_flattened() {
    let mut next = noise(7);
    let picture = Picture::new(160, 160, |_, _| {
        let v = 175 + (next() % 51) as u8;
        [v, v - 20, v - 40, 255]
    });
    let (solver, out) = erase_rect(&picture, 60, 60, 40, 40);
    assert_eq!(solver, Solver::Patches);
    let values: Vec<f32> = out.chunks(4).map(|p| f32::from(p[0])).collect();
    let mean = values.iter().sum::<f32>() / values.len() as f32;
    let deviation = (values.iter().map(|v| (v - mean).powi(2)).sum::<f32>() / values.len() as f32).sqrt();
    // The source is uniform on 175..=225: mean 200, standard deviation about 14.7.
    assert!((mean - 200.0).abs() < 8.0, "mean {mean}");
    assert!(deviation > 7.0, "deviation {deviation}");
}

#[test]
fn bgra_input_gives_the_same_pixels_as_rgba() {
    let mut next = noise(11);
    let rgba = Picture::new(120, 120, |x, _| {
        let n = (next() % 40) as u8;
        [150 + n, 60 + (x as u8 % 30), 30 + n / 2, 255]
    });
    let mut bgra = Picture { width: rgba.width, height: rgba.height, rgba: rgba.rgba.clone() };
    for p in bgra.rgba.chunks_mut(4) {
        p.swap(0, 2);
    }
    let mask = rect_mask(30, 30);
    let hole = Hole { x: 45, y: 45, width: 30, height: 30, mask: &mask };
    let mut a = vec![0; 30 * 30 * 4];
    let mut b = a.clone();
    let solver_a = erase(&rgba.image(ChannelOrder::Rgba), &hole, &|| false, &mut a).unwrap();
    let solver_b = erase(&bgra.image(ChannelOrder::Bgra), &hole, &|| false, &mut b).unwrap();
    assert_eq!(solver_a, solver_b);
    for p in b.chunks_mut(4) {
        p.swap(0, 2);
    }
    assert_eq!(a, b);
}

#[test]
fn only_masked_opaque_pixels_change() {
    let picture = Picture::new(100, 80, |x, y| {
        let alpha = if (40..50).contains(&x) && (30..40).contains(&y) { 0 } else { 255 };
        [10 + x as u8, 20 + y as u8, 200, alpha]
    });
    // Erase a diagonal band only.
    let (w, h) = (60, 40);
    let mask: Vec<u8> = (0..w * h).map(|i: usize| u8::from((i % w).abs_diff(i / w + 10) < 6)).collect();
    let mut out = vec![0; w * h * 4];
    erase(&picture.image(ChannelOrder::Rgba), &Hole { x: 20, y: 20, width: w, height: h, mask: &mask }, &|| false, &mut out).unwrap();
    for y in 0..h {
        for x in 0..w {
            let original = &picture.rgba[((20 + y) * 100 + 20 + x) * 4..][..4];
            let p = pixel(&out, w, x, y);
            if mask[y * w + x] == 0 || original[3] == 0 {
                assert_eq!(&p[..], original, "at {x},{y}");
            }
        }
    }
}

#[test]
fn cancelling_stops_the_solve() {
    let picture = Picture::new(64, 64, |_, _| [200, 200, 200, 255]);
    let mask = rect_mask(10, 10);
    let mut out = vec![0; 400];
    let result = erase(&picture.image(ChannelOrder::Rgba), &Hole { x: 20, y: 20, width: 10, height: 10, mask: &mask }, &|| true, &mut out);
    assert_eq!(result, Err(EraseError::Cancelled));
}

#[test]
fn an_empty_mask_leaves_the_pixels_unchanged() {
    let picture = Picture::new(32, 32, |x, y| [x as u8, y as u8, 7, 255]);
    let mask = vec![0; 100];
    let mut out = vec![0; 400];
    let solver = erase(&picture.image(ChannelOrder::Rgba), &Hole { x: 5, y: 5, width: 10, height: 10, mask: &mask }, &|| false, &mut out).unwrap();
    assert_eq!(solver, Solver::Unchanged);
    assert_eq!(pixel(&out, 10, 3, 4), [8, 9, 7, 255]);
}

#[test]
fn mismatched_sizes_are_rejected() {
    let picture = Picture::new(32, 32, |_, _| [0, 0, 0, 255]);
    let image = picture.image(ChannelOrder::Rgba);
    let mask = rect_mask(10, 10);
    let mut out = vec![0; 400];
    let outside = Hole { x: 25, y: 0, width: 10, height: 10, mask: &mask };
    assert_eq!(erase(&image, &outside, &|| false, &mut out), Err(EraseError::InvalidInput));
    let short_mask = Hole { x: 0, y: 0, width: 10, height: 10, mask: &mask[..99] };
    assert_eq!(erase(&image, &short_mask, &|| false, &mut out), Err(EraseError::InvalidInput));
    let fits = Hole { x: 0, y: 0, width: 10, height: 10, mask: &mask };
    assert_eq!(erase(&image, &fits, &|| false, &mut out[..399]), Err(EraseError::InvalidInput));
    let truncated = Image { pixels: &picture.rgba[..picture.rgba.len() - 1], ..picture.image(ChannelOrder::Rgba) };
    assert_eq!(erase(&truncated, &fits, &|| false, &mut out), Err(EraseError::InvalidInput));
}

#[test]
fn a_hole_on_the_image_edge_is_filled_from_one_side() {
    let picture = Picture::new(120, 80, |x, _| [40 + x as u8, 90, 140, 255]);
    let (solver, out) = erase_rect(&picture, 0, 0, 30, 80);
    assert_eq!(solver, Solver::Surface);
    for y in 0..80 {
        for x in 0..30 {
            assert!(pixel(&out, 30, x, y)[0].abs_diff(40 + x as u8) <= 1, "at {x},{y}");
        }
    }
}

#[test]
fn a_hole_too_large_for_one_pass_is_solved_reduced() {
    // The region exceeds the working budget, so it is solved at half size and enlarged.
    let picture = Picture::new(1800, 1200, |x, y| [(x / 8) as u8, (y / 6) as u8, 128, 255]);
    let (w, h) = (1400, 900);
    let (solver, out) = erase_rect(&picture, 200, 150, w, h);
    assert_eq!(solver, Solver::Surface);
    for y in (0..h).step_by(37) {
        for x in (0..w).step_by(41) {
            let p = pixel(&out, w, x, y);
            let expected = [((200 + x) / 8) as u8, ((150 + y) / 6) as u8];
            // The source is a staircase; the fitted plane runs through its middle.
            assert!(p[0].abs_diff(expected[0]) <= 1 && p[1].abs_diff(expected[1]) <= 1, "{p:?} vs {expected:?} at {x},{y}");
        }
    }
}

#[test]
fn the_working_budget_picks_the_smallest_reduction() {
    assert_eq!(reduction([0, 0, 1024, 1024]), 1);
    assert_eq!(reduction([0, 0, 1025, 1024]), 2);
    assert_eq!(reduction([0, 0, 3000, 2000]), 3);
}
