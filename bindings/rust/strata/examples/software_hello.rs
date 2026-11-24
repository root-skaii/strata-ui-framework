use strata::{window_flags, Color, Context, Input, SoftwareRenderer, Vec2};

fn main() {
    let mut ctx = Context::with_defaults().expect("failed to create strata context");
    let mut sw = SoftwareRenderer::new(&ctx).expect("failed to create software renderer");
    sw.resize(800, 480);

    let mut enabled = true;
    let mut gain = 0.6f32;
    let mut lo = 0.25f32;
    let mut hi = 0.75f32;
    let input = Input {
        display_size: Vec2::new(800.0, 480.0),
        ..Input::default()
    };

    ctx.frame(&input, |ui| {
        ui.window("strata + rust", Vec2::new(32.0, 32.0), Vec2::new(360.0, 240.0), window_flags::NONE, |ui| {
            ui.text("Immediate-mode UI rendered through strata_c.h");
            ui.checkbox("Enabled", &mut enabled);
            ui.range_slider("Range", &mut lo, &mut hi, 0.0, 1.0);
            ui.knob("Gain", &mut gain, 0.0, 1.0, Some(0.5));
        });
    });

    sw.clear(Color::rgb(14, 16, 22));
    sw.render(&ctx);
    let stats = ctx.stats();
    println!(
        "Rendered {} vertices across {} draw calls ({:.2} ms)",
        stats.vertices, stats.draw_calls, stats.ui_ms
    );
}
