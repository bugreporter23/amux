use std::io;

use serde::{Deserialize, Serialize};

use super::{Cursor, Grid, Row, Storage};
use crate::index::Line;
use crate::term::cell::Cell;

#[derive(Serialize, Deserialize)]
pub struct GridState {
    rows: Vec<RowState>,
    columns: usize,
    lines: usize,
    display_offset: usize,
    max_scroll_limit: usize,
    amux_next: u64,
    amux_epoch: u64,
}

#[derive(Serialize, Deserialize)]
struct RowState {
    runs: Vec<(Cell, String)>,
    occ: usize,
}

impl GridState {
    pub fn capture(grid: &Grid<Cell>) -> Self { Self::capture_rows(grid, grid.raw.len()) }

    fn capture_rows(grid: &Grid<Cell>, count: usize) -> Self {
        let rows = (0..count).map(|index| {
            let row = &grid.raw[Line(grid.lines as i32 - 1 - index as i32)];
            let runs = crate::term::history::encode_cells(&row[..]);
            RowState { runs, occ: row.occ }
        }).collect();
        Self { rows, columns: grid.columns, lines: grid.lines,
               display_offset: grid.display_offset, max_scroll_limit: grid.max_scroll_limit,
               amux_next: grid.amux_next, amux_epoch: grid.amux_epoch }
    }

    pub fn screen(grid: &Grid<Cell>) -> Self {
        let mut state = Self::capture_rows(grid, grid.lines);
        state.display_offset = 0;
        state.max_scroll_limit = 0;
        state
    }

    pub fn restore(self) -> io::Result<Grid<Cell>> {
        if self.columns == 0 || self.lines == 0 || self.rows.len() < self.lines
            || self.rows.len() - self.lines > self.max_scroll_limit
            || self.display_offset > self.rows.len() - self.lines {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid grid checkpoint"));
        }
        let mut rows = Vec::with_capacity(self.rows.len());
        for state in self.rows {
            let mut row = Row::new(self.columns);
            let mut column = 0;
            for (template, text) in state.runs {
                for c in text.chars() {
                    if column >= self.columns {
                        return Err(io::Error::new(io::ErrorKind::InvalidData, "long checkpoint row"));
                    }
                    row.inner[column] = Cell { c, ..template.clone() };
                    column += 1;
                }
            }
            if column != self.columns || state.occ > self.columns {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid checkpoint row"));
            }
            row.occ = state.occ;
            rows.push(row);
        }
        let len = rows.len();
        Ok(Grid {
            cursor: Cursor::default(), saved_cursor: Cursor::default(),
            raw: Storage { inner: rows, zero: 0, visible_lines: self.lines, len },
            amux_next: self.amux_next, amux_epoch: self.amux_epoch,
            columns: self.columns, lines: self.lines,
            display_offset: self.display_offset, max_scroll_limit: self.max_scroll_limit,
        })
    }
}
