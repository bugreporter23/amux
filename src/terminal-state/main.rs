use std::cell::RefCell;
use std::io::{self, Read, Write};
use std::rc::Rc;
use std::time::Instant;

use alacritty_terminal::event::{Event, EventListener, WindowSize};
use alacritty_terminal::grid::Dimensions;
use alacritty_terminal::term::checkpoint::{self, Size};
use alacritty_terminal::term::{Config, Term};
use alacritty_terminal::vte::ansi::{Processor, Rgb};

#[derive(Clone, Default)]
struct Events(Rc<RefCell<Vec<Event>>>);
impl EventListener for Events {
    fn send_event(&self, event: Event) { self.0.borrow_mut().push(event); }
}

fn default_color(index: usize) -> Option<Rgb> {
    const COLORS: [u32; 16] = [0x181818, 0xac4242, 0x90a959, 0xf4bf75, 0x6a9fb5, 0xaa759f,
        0x75b5aa, 0xd8d8d8, 0x6b6b6b, 0xc55555, 0xaac474, 0xfeca88, 0x82b8c8, 0xc28cb8, 0x93d3c3, 0xf8f8f8];
    let rgb = match index {
        0..=15 => COLORS[index],
        16..=231 => {
            let i = index - 16;
            let component = |v| if v == 0 { 0 } else { 55 + v * 40 };
            (component(i / 36) << 16 | component(i / 6 % 6) << 8 | component(i % 6)) as u32
        },
        232..=255 => ((index - 232) as u32 * 10 + 8) * 0x010101,
        256 | 267 => 0xd8d8d8,
        257 => 0x181818,
        259..=266 => [0x0f0f0f, 0x712b2b, 0x5f6f3a, 0xa17e4d, 0x456877, 0x704d68, 0x4d7770, 0x8e8e8e][index - 259],
        268 => 0x8e8e8e,
        _ => return None,
    };
    Some(Rgb { r: (rgb >> 16) as u8, g: (rgb >> 8) as u8, b: rgb as u8 })
}

fn responses(term: &Term<Events>, events: &Events) -> Vec<u8> {
    let mut result = Vec::new();
    for event in events.0.borrow_mut().drain(..) {
        let text = match event {
            Event::PtyWrite(text) => Some(text),
            Event::ColorRequest(index, format) => term.colors()[index].or_else(|| default_color(index)).map(|color| format(color)),
            Event::TextAreaSizeRequest(format) => Some(format(WindowSize {
                num_cols: term.columns() as u16, num_lines: term.screen_lines() as u16,
                cell_width: 0, cell_height: 0,
            })),
            _ => None,
        };
        if let Some(text) = text { result.extend_from_slice(text.as_bytes()); }
    }
    result
}

fn run() -> io::Result<()> {
    let events = Events::default();
    let mut term = Term::new(Config { kitty_keyboard: true, ..Config::default() }, &Size(80, 24), events.clone());
    let mut parser: Processor = Processor::new();
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    loop {
        let mut header = [0; 5];
        match input.read_exact(&mut header) {
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            result => result?,
        }
        let mut data = vec![0; u32::from_be_bytes(header[1..].try_into().unwrap()) as usize];
        input.read_exact(&mut data)?;
        let expired = matches!(header[0], b'D' | b'R') && parser.sync_timeout().sync_timeout().is_some_and(|deadline| deadline <= Instant::now());
        if expired {
            parser.stop_sync(&mut term);
        }
        match header[0] {
            b'D' => parser.advance(&mut term, &data),
            b'R' => {
                let [columns, rows]: [usize; 2] = serde_json::from_slice(&data).map_err(io::Error::other)?;
                term.resize(Size(columns, rows));
            },
            b'C' | b'L' | b'H' | b'T' => {},
            b'X' => term.amux_clear_history(),
            b'I' => checkpoint::restore(&mut term, &mut parser, &data)?,
            _ => return Err(io::Error::new(io::ErrorKind::InvalidData, "unknown state command")),
        }
        if matches!(header[0], b'R' | b'X') || (header[0] == b'D' && (!data.is_empty() || expired)) { term.amux_history.sequence += 1; }
        let replies = responses(&term, &events);
        output.write_all(&(replies.len() as u32).to_be_bytes())?;
        output.write_all(&replies)?;
        let state = match header[0] {
            b'D' if expired => checkpoint::capture_live(&term, &parser)?,
            b'C' => checkpoint::capture(&term, &parser)?,
            b'L' | b'X' => checkpoint::capture_live(&term, &parser)?,
            b'T' => {
                let history: usize = serde_json::from_slice(&data).map_err(io::Error::other)?;
                let value = match term.amux_capture(history) {
                    Ok(text) => serde_json::json!({"text": text}),
                    Err(error) => serde_json::json!({"error": error.to_string()}),
                };
                serde_json::to_vec(&value).map_err(io::Error::other)?
            },
            b'H' => match term.history_read(&data) {
                Ok(page) => page,
                Err(error) => serde_json::to_vec(&serde_json::json!({"error": error.to_string()})).map_err(io::Error::other)?,
            },
            _ => Vec::new(),
        };
        output.write_all(&(state.len() as u32).to_be_bytes())?;
        output.write_all(&state)?;
        let timeout = parser.sync_timeout().sync_timeout().map_or(0, |deadline| {
            (deadline.saturating_duration_since(Instant::now()).as_millis() + 1) as u32
        });
        output.write_all(&timeout.to_be_bytes())?;
        let descriptor = serde_json::to_vec(&term.history_descriptor()).map_err(io::Error::other)?;
        output.write_all(&(descriptor.len() as u32).to_be_bytes())?;
        output.write_all(&descriptor)?;
        output.flush()?;
    }
}

fn main() {
    if let Err(error) = run() { eprintln!("terminal state: {error}"); std::process::exit(1); }
}
