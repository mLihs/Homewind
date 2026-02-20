#!/usr/bin/env python3
"""
WebUI File Watcher - Automatically rebuild WebUI when source files change.
Watches webui_src/ and runs build_webui.py on any changes.

Uses polling (no external dependencies required).
"""

import sys
import time
import subprocess
from pathlib import Path


class WebUIPollWatcher:
    """Poll file system for changes and trigger rebuilds"""
    
    def __init__(self, source_dir, output_dir, build_script, poll_interval=0.5):
        self.source_dir = Path(source_dir)
        self.output_dir = Path(output_dir)
        self.build_script = Path(build_script)
        self.poll_interval = poll_interval
        
        # Files to watch
        self.watched_files = {
            'index.html': None,
            'app.css': None,
            'app.js': None
        }
        
        # Initialize file timestamps
        self.update_file_timestamps()
    
    def update_file_timestamps(self):
        """Update stored modification times for watched files"""
        for filename in self.watched_files.keys():
            file_path = self.source_dir / filename
            if file_path.exists():
                self.watched_files[filename] = file_path.stat().st_mtime
            else:
                self.watched_files[filename] = None
    
    def check_for_changes(self):
        """Check if any watched files have changed"""
        for filename, old_mtime in self.watched_files.items():
            file_path = self.source_dir / filename
            if file_path.exists():
                current_mtime = file_path.stat().st_mtime
                if old_mtime is None or current_mtime != old_mtime:
                    return True
        return False
    
    def trigger_build(self):
        """Run the build script"""
        print(f"\n{'='*60}")
        print(f"File change detected - Rebuilding WebUI...")
        print(f"{'='*60}\n")
        
        try:
            # Run the build script
            result = subprocess.run(
                [sys.executable, str(self.build_script), 
                 str(self.source_dir), str(self.output_dir)],
                cwd=self.build_script.parent.parent,
                check=True
            )
            print(f"\n✓ Rebuild complete at {time.strftime('%H:%M:%S')}\n")
            return True
        except subprocess.CalledProcessError as e:
            print(f"\n✗ Build failed with exit code {e.returncode}\n")
            return False
        except Exception as e:
            print(f"\n✗ Build error: {e}\n")
            return False
    
    def watch(self):
        """Start watching for changes"""
        print("="*60)
        print("WebUI File Watcher (Polling Mode)")
        print("="*60)
        print(f"Watching: {self.source_dir}")
        print(f"Output:   {self.output_dir}")
        print(f"Build:    {self.build_script}")
        print(f"Poll interval: {self.poll_interval}s")
        print("\nWatching for changes to: index.html, app.css, app.js")
        print("Press Ctrl+C to stop\n")
        
        # Do an initial build
        print("Running initial build...\n")
        self.trigger_build()
        self.update_file_timestamps()
        
        try:
            while True:
                time.sleep(self.poll_interval)
                
                if self.check_for_changes():
                    self.trigger_build()
                    self.update_file_timestamps()
        except KeyboardInterrupt:
            print("\n\nStopping file watcher...")
            print("File watcher stopped.")


def main():
    """Main entry point"""
    # Get script directory
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    
    # Set up paths
    source_dir = project_root / 'webui_src'
    output_dir = project_root / 'generated'
    build_script = script_dir / 'build_webui.py'
    
    # Verify paths exist
    if not source_dir.exists():
        print(f"ERROR: Source directory not found: {source_dir}")
        sys.exit(1)
    
    if not build_script.exists():
        print(f"ERROR: Build script not found: {build_script}")
        sys.exit(1)
    
    # Create output directory if it doesn't exist
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Start watcher
    watcher = WebUIPollWatcher(source_dir, output_dir, build_script)
    watcher.watch()


if __name__ == '__main__':
    main()
