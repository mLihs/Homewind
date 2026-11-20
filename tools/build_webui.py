#!/usr/bin/env python3
"""
WebUI Build Pipeline - Generate PROGMEM headers from webui_src/
Deterministic GZIP compression and header generation.
"""

import hashlib
import os
import sys
import re
import subprocess
import tempfile
from pathlib import Path

# Asset manifest entry structure
ASSET_MANIFEST_ENTRY = """  {{
    .path = "{path}",
    .data = {var_name}_data,
    .len = {var_name}_len,
    .gzipped = {gzipped},
    .mime_type = "{mime_type}",
    .etag = "\\"{etag}\\"",
    .cacheSeconds = {cache_seconds}
  }}"""

def get_mime_type(path):
    """Determine MIME type from file extension"""
    ext = Path(path).suffix.lower()
    mime_types = {
        '.html': 'text/html',
        '.css': 'text/css',
        '.js': 'application/javascript',
        '.svg': 'image/svg+xml',
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.gif': 'image/gif',
        '.ico': 'image/x-icon',
        '.json': 'application/json',
        '.txt': 'text/plain'
    }
    return mime_types.get(ext, 'application/octet-stream')

def get_cache_seconds(path):
    """Determine cache duration from file type"""
    ext = Path(path).suffix.lower()
    # HTML files: 0 = no-cache (revalidate via ETag)
    if ext == '.html':
        return 0
    # Images and other static assets: 7 days
    else:
        return 604800

def calculate_etag(data):
    """Calculate simple ETag from data hash"""
    # Content hash; Python's hash() is salted per process and not reproducible
    return hashlib.sha256(data).hexdigest()[:8]

def run_on_temp_file(data, suffix, command):
    """Write data to a temp file, run command + [path], return stdout bytes or None"""
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp.write(data)
        path = tmp.name
    try:
        result = subprocess.run(command + [path], capture_output=True, timeout=60)
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        print(f"  Warning: {command[0]} failed: {e}")
        return None
    finally:
        os.unlink(path)
    if result.returncode != 0:
        print(f"  Warning: {command[0]} failed: {result.stderr.decode(errors='replace')}")
        return None
    return result.stdout

def compress_gzip(data):
    """Compress data to GZIP with Zopfli (~5 % smaller than gzip -6, deterministic)"""
    # brew install zopfli
    return run_on_temp_file(data, '.html', ['zopfli', '-c', '--i15'])

def minify_css(css_content):
    """Minify CSS with Lightning CSS via npx"""
    # Pinned: a new release changes the output and the build hash
    out = run_on_temp_file(css_content.encode('utf-8'), '.css',
                           ['npx', '--yes', 'lightningcss-cli@1.30.1', '--minify'])
    return out.decode('utf-8') if out is not None else None

def remove_html_comments(content):
    """Remove HTML comments (<!-- ... -->) from content"""
    # Remove HTML comments, handling multi-line comments
    # This regex matches <!-- ... --> including newlines
    pattern = r'<!--.*?-->'
    return re.sub(pattern, '', content, flags=re.DOTALL)

def minify_js_with_terser(js_content):
    """
    Minify JavaScript using Terser via npx.
    
    Args:
        js_content: JavaScript source code as string
    
    Returns:
        Minified JavaScript or None if Terser is not available
    """
    try:
        # Create temporary file for input
        with tempfile.NamedTemporaryFile(mode='w', suffix='.js', delete=False, encoding='utf-8') as tmp_in:
            tmp_in.write(js_content)
            tmp_in_path = tmp_in.name
        
        # Create temporary file for output
        with tempfile.NamedTemporaryFile(mode='r', suffix='.js', delete=False, encoding='utf-8') as tmp_out:
            tmp_out_path = tmp_out.name
        
        # Run Terser with recommended settings (including comment removal)
        result = subprocess.run(
            [
                # Pinned: a new Terser release changes the output and the build hash
                'npx', '--yes', 'terser@5.51.3',
                tmp_in_path,
                '--ecma', '2020',
                '--compress', 'passes=3',
                '--mangle',
                # app.js is one classic script; shared names go through window.*
                '--toplevel',
                '--format', 'comments=false',
                '-o', tmp_out_path
            ],
            capture_output=True,
            text=True,
            timeout=30
        )
        
        # Clean up input file
        os.unlink(tmp_in_path)
        
        if result.returncode == 0:
            # Read minified output
            with open(tmp_out_path, 'r', encoding='utf-8') as f:
                minified = f.read()
            os.unlink(tmp_out_path)
            return minified
        else:
            # Clean up output file on error
            if os.path.exists(tmp_out_path):
                os.unlink(tmp_out_path)
            print(f"  Warning: Terser failed: {result.stderr}")
            return None
    except FileNotFoundError:
        print("  Warning: npx not found")
        return None
    except subprocess.TimeoutExpired:
        print("  Warning: Terser timed out")
        if os.path.exists(tmp_in_path):
            os.unlink(tmp_in_path)
        if os.path.exists(tmp_out_path):
            os.unlink(tmp_out_path)
        return None
    except Exception as e:
        print(f"  Warning: Terser error: {e}")
        if 'tmp_in_path' in locals() and os.path.exists(tmp_in_path):
            os.unlink(tmp_in_path)
        if 'tmp_out_path' in locals() and os.path.exists(tmp_out_path):
            os.unlink(tmp_out_path)
        return None

def bytes_to_c_array(data, var_name, line_width=80):
    """Convert bytes to C array format"""
    lines = []
    current_line = f"const uint8_t {var_name}[] PROGMEM = {{"
    bytes_per_line = 16  # Hex bytes per line
    
    for i, byte_val in enumerate(data):
        if i % bytes_per_line == 0 and i > 0:
            lines.append(current_line)
            current_line = "  "
        
        hex_str = f"0x{byte_val:02x}"
        if i < len(data) - 1:
            hex_str += ","
        else:
            hex_str += "  // "
        
        # Check if adding this byte would exceed line width
        test_line = current_line + hex_str
        if len(test_line) > line_width and i % bytes_per_line != 0:
            lines.append(current_line.rstrip())
            current_line = "  " + hex_str
        else:
            current_line += hex_str
    
    if current_line.strip():
        lines.append(current_line.rstrip())
    
    lines.append("};")
    return '\n'.join(lines)

def build_webui(source_dir, output_dir):
    """Build the WebUI into one PROGMEM asset: index.html with app.css and app.js inlined"""
    source_path = Path(source_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    print("=== WebUI Build Pipeline ===\n")
    
    sources = {}
    for name in ('index.html', 'app.css', 'app.js'):
        path = source_path / name
        if not path.exists():
            print(f"ERROR: Missing source file: {name}")
            return False
        with open(path, 'r', encoding='utf-8') as f:
            sources[name] = f.read()
    
    html = sources['index.html']
    for marker in ('__WEBUI_CSS__', '__WEBUI_JS__'):
        if html.count(marker) != 1:
            print(f"ERROR: index.html must contain {marker} exactly once")
            return False
    
    print("Minifying app.js with Terser...")
    js = minify_js_with_terser(sources['app.js'])
    if js is None:
        # Unminified JS costs about 31 KB more flash
        print("Error: Terser failed, app.js not minified; WebUI not built")
        return False
    print("Minifying app.css with Lightning CSS...")
    css = minify_css(sources['app.css'])
    if css is None:
        print("Error: Lightning CSS failed, app.css not minified; WebUI not built")
        return False
    
    # The browser would end the inline element at the first closing tag
    for name, code, tag in (('app.js', js, '</script'), ('app.css', css, '</style')):
        if tag in code.lower():
            print(f"ERROR: {name} contains '{tag}' and cannot be inlined")
            return False
    
    # Comments are stripped before inlining, so "<!--" inside JS strings stays untouched
    page = remove_html_comments(html)
    page = page.replace('__WEBUI_CSS__', css).replace('__WEBUI_JS__', js)
    build_hash = calculate_etag(page.encode('utf-8'))
    page = page.replace('__WEBUI_HASH__', build_hash)
    
    raw_data = page.encode('utf-8')
    print("Compressing with Zopfli...")
    compressed_data = compress_gzip(raw_data)
    if compressed_data is None:
        print("Error: Zopfli failed (brew install zopfli); WebUI not built")
        return False
    use_compressed = len(compressed_data) < len(raw_data)
    final_data = compressed_data if use_compressed else raw_data
    
    asset_file = 'index.html'
    var_name = "webui_index_html"
    header_content = f"""/**
 * @file {var_name}.h
 * @brief Auto-generated WebUI asset: {asset_file} (app.css and app.js inlined)
 * @warning DO NOT EDIT - Generated by build_webui.py
 */

#ifndef HOMEWIND_WEBUI_INDEX_HTML_H
#define HOMEWIND_WEBUI_INDEX_HTML_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

{bytes_to_c_array(final_data, f"{var_name}_data")}

const size_t {var_name}_len = {len(final_data)};

#endif // HOMEWIND_WEBUI_INDEX_HTML_H
"""
    header_file = output_path / f"{var_name}.h"
    with open(header_file, 'w', encoding='utf-8') as f:
        f.write(header_content)
    print(f"✓ Generated {header_file.name}")
    
    # Headers of the former separate CSS/JS assets
    for stale in ('webui_app_css.h', 'webui_app_js.h'):
        if (output_path / stale).exists():
            (output_path / stale).unlink()
            print(f"✓ Removed {stale}")
    
    # Generate web_assets.h (includes all asset headers)
    assets_header = f"""/**
 * @file web_assets.h
 * @brief Auto-generated WebUI assets header
 * @warning DO NOT EDIT - Generated by build_webui.py
 */

#ifndef HOMEWIND_WEB_ASSETS_H
#define HOMEWIND_WEB_ASSETS_H

#include "{var_name}.h"

#endif // HOMEWIND_WEB_ASSETS_H
"""
    
    assets_header_file = output_path / 'web_assets.h'
    with open(assets_header_file, 'w', encoding='utf-8') as f:
        f.write(assets_header)
    
    print(f"✓ Generated {assets_header_file.name}")
    
    manifest_entry = ASSET_MANIFEST_ENTRY.format(
        path=asset_file,
        var_name=var_name,
        gzipped='true' if use_compressed else 'false',
        mime_type=get_mime_type(asset_file),
        etag=calculate_etag(final_data),
        cache_seconds=get_cache_seconds(asset_file)
    )
    
    # Generate web_assets_manifest.h (asset lookup table)
    manifest_header = f"""/**
 * @file web_assets_manifest.h
 * @brief Auto-generated WebUI asset manifest
 * @warning DO NOT EDIT - Generated by build_webui.py
 */

#ifndef HOMEWIND_WEB_ASSETS_MANIFEST_H
#define HOMEWIND_WEB_ASSETS_MANIFEST_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @struct WebAsset
 * @brief Single web asset entry
 */
struct WebAsset {{
  const char* path;
  const uint8_t* data;
  size_t len;
  bool gzipped;
  const char* mime_type;
  const char* etag;
  uint32_t cacheSeconds;
}};

/**
 * @brief WebUI asset manifest (array of all assets)
 */
static const WebAsset WEB_ASSETS[] PROGMEM = {{
{manifest_entry}
}};

/**
 * @brief Number of assets in manifest
 */
static const size_t WEB_ASSETS_COUNT = 1;

/**
 * @brief WebUI build hash (also in index.html as data-webui-build)
 */
#define HOMEWIND_WEBUI_BUILD_HASH "{build_hash}"

#endif // HOMEWIND_WEB_ASSETS_MANIFEST_H
"""
    
    manifest_header_file = output_path / 'web_assets_manifest.h'
    with open(manifest_header_file, 'w', encoding='utf-8') as f:
        f.write(manifest_header)
    
    print(f"✓ Generated {manifest_header_file.name}")
    
    print(f"\n=== Build Summary ===")
    for name in ('index.html', 'app.css', 'app.js'):
        print(f"{name}: {len(sources[name].encode('utf-8')):,} bytes source")
    print(f"index.html (inlined, minified): {len(raw_data):,} bytes")
    print(f"Embedded: {len(final_data):,} bytes ({'gzipped' if use_compressed else 'raw'})")
    print(f"Build hash: {build_hash}")
    print(f"Output directory: {output_path.absolute()}")
    print("\n✓ Build complete!")
    
    return True

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: build_webui.py <source_dir> <output_dir>")
        print("  source_dir: Directory containing webui_src/ (index.html, app.css, app.js)")
        print("  output_dir: Directory for generated headers (src/generated/)")
        print("")
        print("Example: python3 tools/build_webui.py webui_src src/generated")
        sys.exit(1)
    
    success = build_webui(sys.argv[1], sys.argv[2])
    sys.exit(0 if success else 1)

