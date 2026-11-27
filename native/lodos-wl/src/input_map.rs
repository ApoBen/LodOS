//! Android input constants translated to the compositor's existing evdev/control path.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TouchAction {
    Down,
    Motion,
    Up,
    Cancel,
}

pub fn touch_action(action: i32) -> Option<TouchAction> {
    match action {
        0 | 5 => Some(TouchAction::Down),
        1 | 6 => Some(TouchAction::Up),
        2 => Some(TouchAction::Motion),
        3 => Some(TouchAction::Cancel),
        _ => None,
    }
}

/// Android KeyEvent codes used by common physical and soft keyboards -> Linux evdev.
pub fn evdev_key(code: i32) -> Option<u32> {
    let mapped = match code {
        4 => 1,                          // BACK / Escape
        7..=16 => 2 + (code - 7) as u32, // digits
        19 => 103,
        20 => 108,
        21 => 105,
        22 => 106,
        29..=54 => {
            const LETTERS: [u32; 26] = [
                30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22,
                47, 17, 45, 21, 44,
            ];
            LETTERS[(code - 29) as usize]
        }
        55 => 51,
        56 => 52,
        57 | 58 => 42,
        59 | 60 => 54,
        61 => 15,
        62 => 57,
        66 => 28,
        67 => 14,
        111 => 1,
        112 => 111,
        _ => return None,
    };
    Some(mapped)
}

/// Scale view pixel coordinates into the compositor's logical output dimensions.
pub fn scale_coords(
    x: f32,
    y: f32,
    surface_w: f32,
    surface_h: f32,
    output_w: f32,
    output_h: f32,
) -> Option<(f64, f64)> {
    if ![x, y, surface_w, surface_h, output_w, output_h]
        .iter()
        .all(|v| v.is_finite())
        || surface_w <= 0.0
        || surface_h <= 0.0
    {
        return None;
    }
    Some((
        (x * output_w / surface_w) as f64,
        (y * output_h / surface_h) as f64,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn down_and_pointer_down_map_to_down() {
        assert_eq!(touch_action(0), Some(TouchAction::Down));
        assert_eq!(touch_action(5), Some(TouchAction::Down));
    }
    #[test]
    fn motion_maps() {
        assert_eq!(touch_action(2), Some(TouchAction::Motion));
    }
    #[test]
    fn up_and_pointer_up_map_to_up() {
        assert_eq!(touch_action(1), Some(TouchAction::Up));
        assert_eq!(touch_action(6), Some(TouchAction::Up));
    }
    #[test]
    fn cancel_maps() {
        assert_eq!(touch_action(3), Some(TouchAction::Cancel));
    }
    #[test]
    fn unknown_action_ignored() {
        assert_eq!(touch_action(99), None);
    }
    #[test]
    fn letter_keycode_maps() {
        assert_eq!(evdev_key(29), Some(30));
        assert_eq!(evdev_key(36), Some(35));
    }
    #[test]
    fn unknown_keycode_ignored() {
        assert_eq!(evdev_key(999), None);
    }
    #[test]
    fn coordinates_scale() {
        assert_eq!(
            scale_coords(50.0, 25.0, 100.0, 50.0, 1080.0, 2400.0),
            Some((540.0, 1200.0))
        );
    }
    #[test]
    fn zero_surface_rejected() {
        assert_eq!(scale_coords(1.0, 1.0, 0.0, 2.0, 3.0, 4.0), None);
    }
}
