//! Thimsy universal installer engine.
//!
//! Everything the app does besides drawing windows lives here: the device and
//! profile catalog, plan resolution, downloads, Thimsy packages, firmware
//! container editing (Unisoc PAC, Android `super`), deltas, the job journal, USB
//! detection and running the bundled flashing tools.

pub mod archive;
pub mod bundle;
pub mod catalog;
pub mod cli;
pub mod delta;
pub mod detect;
pub mod disk;
pub mod drivers;
pub mod error;
pub mod events;
pub mod fetch;
pub mod flash;
pub mod host;
pub mod i18n;
pub mod job;
pub mod lp;
pub mod manifest;
pub mod pac;
pub mod pack;
pub mod package;
pub mod resolve;
pub mod runlog;
pub mod tools;
pub mod util;

#[cfg(test)]
mod tests;
#[cfg(test)]
mod testutil;

pub use error::{Code, EngineError};
pub use events::{Event, Reporter};

/// Engine version, shown in `catalog` output and job locks.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// This installer's version as packages' `min_installer` names it: the
/// engine version, with `-testN` for test N of it (see
/// [`package::parse_version`]). Raised with every test release.
pub const INSTALLER_VERSION: &str = "0.1.0-test22";
