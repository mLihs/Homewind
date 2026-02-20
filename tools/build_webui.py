#!/usr/bin/env python3
"""
WebUI Build Pipeline - Generate PROGMEM headers from webui_src/
Deterministic GZIP compression and header generation.
"""

import gzip
import os
import sys
import re
import subprocess
import tempfile
from pathlib import Path
from datetime import datetime

# Asset manifest entry structure
ASSET_MANIFEST_ENTRY = """  {{
    .path = "{path}",
    .data = {var_name}_data,
    .len = {var_name}_len,
    .gzipped = {gzipped},
    .mime_type = "{mime_type}",
    .etag = "{etag}",
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
    # HTML files: 1 hour (may change frequently)
    if ext == '.html':
        return 3600
    # CSS/JS files: 1 day (change less frequently)
    elif ext in ['.css', '.js']:
        return 86400
    # Images and other static assets: 7 days
    else:
        return 604800

def calculate_etag(data):
    """Calculate simple ETag from data hash"""
    # Simple hash-based ETag (not cryptographically secure, just for caching)
    hash_val = hash(data) & 0xFFFFFFFF
    return f'{hash_val:08x}'

def calculate_build_hash(assets_data):
    """
    Calculate deterministic build hash from all asset data.
    This hash changes when any asset changes, ensuring cache-busting.
    
    Args:
        assets_data: List of final asset data (bytes) in processing order
    
    Returns:
        String hash (8 hex digits)
    """
    # Combine all asset data in deterministic order
    combined = b''.join(assets_data)
    # Use same hash algorithm as ETag for consistency
    hash_val = hash(combined) & 0xFFFFFFFF
    return f'{hash_val:08x}'

def compress_gzip(data):
    """Compress data with GZIP (deterministic: mtime=0)"""
    # Use compresslevel=6 (good balance), mtime=None to make it deterministic
    compressed = gzip.compress(data, compresslevel=6, mtime=0)
    return compressed

def remove_html_comments(content):
    """Remove HTML comments (<!-- ... -->) from content"""
    # Remove HTML comments, handling multi-line comments
    # This regex matches <!-- ... --> including newlines
    pattern = r'<!--.*?-->'
    return re.sub(pattern, '', content, flags=re.DOTALL)

def remove_css_comments(content):
    """Remove CSS comments (/* ... */) from content"""
    # Remove CSS comments, handling multi-line comments
    # This regex matches /* ... */ including newlines
    pattern = r'/\*.*?\*/'
    return re.sub(pattern, '', content, flags=re.DOTALL)

def remove_js_comments(content):
    """Remove JavaScript comments (// and /* ... */) from content"""
    result = []
    i = 0
    in_string = None  # Track if we're inside a string: '"', "'", or None
    in_regex = False  # Track if we might be in a regex literal
    
    while i < len(content):
        char = content[i]
        
        # Handle string literals (don't process comments inside strings)
        if in_string is None:
            if char == '"' or char == "'":
                in_string = char
                result.append(char)
                i += 1
                continue
        else:
            if char == in_string:
                # Check for escaped quote
                if i > 0 and content[i-1] == '\\':
                    result.append(char)
                    i += 1
                    continue
                # End of string
                in_string = None
                result.append(char)
                i += 1
                continue
            result.append(char)
            i += 1
            continue
        
        # Check for single-line comment (//)
        if i < len(content) - 1 and content[i:i+2] == '//':
            # Skip until end of line
            while i < len(content) and content[i] != '\n':
                i += 1
            # Keep the newline
            if i < len(content):
                result.append('\n')
                i += 1
            continue
        
        # Check for multi-line comment (/*)
        if i < len(content) - 1 and content[i:i+2] == '/*':
            # Skip until */
            i += 2
            while i < len(content) - 1:
                if content[i:i+2] == '*/':
                    i += 2
                    break
                i += 1
            continue
        
        # Regular character
        result.append(char)
        i += 1
    
    return ''.join(result)

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
                'npx', '--yes', 'terser',
                tmp_in_path,
                '--ecma', '2020',
                '--compress',
                '--mangle',
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
        print("  Warning: npx not found, JavaScript will remain unminified")
        return None
    except subprocess.TimeoutExpired:
        print("  Warning: Terser timed out, JavaScript will remain unminified")
        if os.path.exists(tmp_in_path):
            os.unlink(tmp_in_path)
        if os.path.exists(tmp_out_path):
            os.unlink(tmp_out_path)
        return None
    except Exception as e:
        print(f"  Warning: Terser error: {e}, JavaScript will remain unminified")
        if 'tmp_in_path' in locals() and os.path.exists(tmp_in_path):
            os.unlink(tmp_in_path)
        if 'tmp_out_path' in locals() and os.path.exists(tmp_out_path):
            os.unlink(tmp_out_path)
        return None

def minify_content(content, file_type):
    """
    Minify content based on file type.
    For JavaScript, uses Terser only. If Terser fails, returns original unminified.
    
    Args:
        content: String content to minify
        file_type: 'html', 'css', or 'js'
    
    Returns:
        Minified content (or original if minification fails for JS)
    """
    if file_type == 'html':
        return remove_html_comments(content)
    elif file_type == 'css':
        return remove_css_comments(content)
    elif file_type == 'js':
        # Use Terser only - if it fails, return original unminified
        terser_result = minify_js_with_terser(content)
        if terser_result is not None:
            return terser_result
        # Return original if Terser is not available (don't use broken comment stripper)
        return content
    else:
        return content

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
    """Build WebUI assets into PROGMEM headers"""
    source_path = Path(source_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    print("=== WebUI Build Pipeline ===\n")
    
    # Asset files to process (in order)
    asset_files = ['index.html', 'app.css', 'app.js']
    
    # Verify all source files exist
    missing = []
    for asset in asset_files:
        if not (source_path / asset).exists():
            missing.append(asset)
    
    if missing:
        print(f"ERROR: Missing source files: {', '.join(missing)}")
        return False
    
    # Process each asset (first pass: without hash replacement for HTML)
    assets = []
    all_headers = []
    all_manifests = []
    assets_final_data = []  # Store final data for build hash calculation
    html_original_content = None  # Store HTML content for hash replacement
    
    for asset_file in asset_files:
        asset_path = source_path / asset_file
        print(f"Processing {asset_file}...")
        
        # Read and minify content
        if asset_file == 'index.html':
            # Read HTML as text (will replace hash placeholder in second pass)
            with open(asset_path, 'r', encoding='utf-8') as f:
                html_original_content = f.read()
            # Remove comments from HTML
            html_minified = minify_content(html_original_content, 'html')
            # For first pass, use minified (will be replaced later)
            raw_data = html_minified.encode('utf-8')
            original_size = len(html_original_content.encode('utf-8'))
        elif asset_file == 'app.css':
            # Read CSS as text
            with open(asset_path, 'r', encoding='utf-8') as f:
                css_content = f.read()
            original_size = len(css_content.encode('utf-8'))
            # Remove comments from CSS
            css_minified = minify_content(css_content, 'css')
            raw_data = css_minified.encode('utf-8')
        elif asset_file == 'app.js':
            # Read JS as text
            with open(asset_path, 'r', encoding='utf-8') as f:
                js_content = f.read()
            original_size = len(js_content.encode('utf-8'))
            # Use Terser only - if not available, keep original unminified
            print("  Attempting to minify with Terser...")
            js_minified = minify_js_with_terser(js_content)
            used_terser = js_minified is not None
            if not used_terser:
                # Keep original if Terser is not available (don't use broken comment stripper)
                js_minified = js_content
            raw_data = js_minified.encode('utf-8')
            minifier_type = "Terser" if used_terser else "unminified"
        else:
            with open(asset_path, 'rb') as f:
                raw_data = f.read()
            original_size = len(raw_data)
        
        # Compress with GZIP
        compressed_data = compress_gzip(raw_data)
        
        # Decide: use compressed if smaller, otherwise raw
        use_compressed = len(compressed_data) < len(raw_data)
        final_data = compressed_data if use_compressed else raw_data
        
        # Store final data for build hash calculation
        assets_final_data.append(final_data)
        
        # Generate variable name (sanitize filename)
        var_base = asset_file.replace('.', '_').replace('-', '_')
        var_name = f"webui_{var_base}"
        
        # Generate C array
        array_code = bytes_to_c_array(final_data, f"{var_name}_data")
        length_code = f"const size_t {var_name}_len = {len(final_data)};"
        
        # Calculate ETag
        etag = calculate_etag(final_data)
        
        # Get MIME type
        mime_type = get_mime_type(asset_file)
        
        # Get cache duration
        cache_seconds = get_cache_seconds(asset_file)
        
        # Generate header content
        header_content = f"""/**
 * @file {var_name}.h
 * @brief Auto-generated WebUI asset: {asset_file}
 * @warning DO NOT EDIT - Generated by build_webui.py
 * @date {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
 */

#ifndef HOMEWIND_WEBUI_{var_base.upper()}_H
#define HOMEWIND_WEBUI_{var_base.upper()}_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

{array_code}

{length_code}

#endif // HOMEWIND_WEBUI_{var_base.upper()}_H
"""
        
        # Write header file
        header_file = output_path / f"{var_name}.h"
        with open(header_file, 'w', encoding='utf-8') as f:
            f.write(header_content)
        
        print(f"  ✓ Generated {header_file.name}")
        if original_size != len(raw_data):
            reduction = original_size - len(raw_data)
            reduction_pct = (reduction / original_size) * 100 if original_size > 0 else 0
            minifier_info = f" ({minifier_type})" if asset_file == 'app.js' and 'minifier_type' in locals() else ""
            print(f"    Size: {original_size} bytes original, {len(raw_data)} bytes after minification{minifier_info} ({reduction} bytes, {reduction_pct:.1f}% reduction)")
        print(f"    Size: {len(raw_data)} bytes raw, {len(compressed_data)} bytes gzipped")
        print(f"    Using: {'gzipped' if use_compressed else 'raw'} ({len(final_data)} bytes)")
        
        # Store asset info for manifest
        assets.append({
            'path': asset_file,
            'var_name': var_name,
            'mime_type': mime_type,
            'etag': etag,
            'gzipped': 'true' if use_compressed else 'false',
            'original_size': original_size,
            'minified_size': len(raw_data),
            'final_size': len(final_data)
        })
        
        all_headers.append(f"#include \"{var_name}.h\"")
        all_manifests.append(ASSET_MANIFEST_ENTRY.format(
            path=asset_file,
            var_name=var_name,
            gzipped='true' if use_compressed else 'false',
            mime_type=mime_type,
            etag=etag,
            cache_seconds=cache_seconds
        ))
    
    # Calculate build hash from all final asset data
    # The hash is calculated from the actual asset content (HTML with placeholder is fine)
    # This ensures the hash changes when any asset content changes
    build_hash = calculate_build_hash(assets_final_data)
    
    # Second pass: Re-process HTML with hash replacement
    # The hash in the HTML URL parameters doesn't affect the hash calculation itself
    # (it's just for browser cache-busting)
    if html_original_content and '__WEBUI_HASH__' in html_original_content:
        print("Re-processing index.html with build hash...")
        # Replace placeholder with actual hash
        html_with_hash = html_original_content.replace('__WEBUI_HASH__', build_hash)
        # Remove comments from HTML (with hash replaced)
        html_minified = minify_content(html_with_hash, 'html')
        html_data = html_minified.encode('utf-8')
        
        # Re-compress HTML with hash replaced
        html_compressed = compress_gzip(html_data)
        html_use_compressed = len(html_compressed) < len(html_data)
        html_final_data = html_compressed if html_use_compressed else html_data
        
        # Regenerate HTML header file with hash-replaced content
        html_var_name = "webui_index_html"
        html_array_code = bytes_to_c_array(html_final_data, f"{html_var_name}_data")
        html_length_code = f"const size_t {html_var_name}_len = {len(html_final_data)};"
        html_etag = calculate_etag(html_final_data)
        
        html_header_content = f"""/**
 * @file {html_var_name}.h
 * @brief Auto-generated WebUI asset: index.html
 * @warning DO NOT EDIT - Generated by build_webui.py
 * @date {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
 */

#ifndef HOMEWIND_WEBUI_INDEX_HTML_H
#define HOMEWIND_WEBUI_INDEX_HTML_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

{html_array_code}

{html_length_code}

#endif // HOMEWIND_WEBUI_INDEX_HTML_H
"""
        
        # Write updated HTML header
        html_header_file = output_path / f"{html_var_name}.h"
        with open(html_header_file, 'w', encoding='utf-8') as f:
            f.write(html_header_content)
        
        print(f"  ✓ Updated {html_header_file.name} with build hash {build_hash}")
        
        # Update manifest entry for HTML
        for i, manifest in enumerate(all_manifests):
            if 'index.html' in manifest:
                all_manifests[i] = ASSET_MANIFEST_ENTRY.format(
                    path='index.html',
                    var_name=html_var_name,
                    gzipped='true' if html_use_compressed else 'false',
                    mime_type='text/html',
                    etag=html_etag,
                    cache_seconds=3600
                )
                break
    
    # Generate web_assets.h (includes all asset headers)
    assets_header = f"""/**
 * @file web_assets.h
 * @brief Auto-generated WebUI assets header
 * @warning DO NOT EDIT - Generated by build_webui.py
 * @date {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
 */

#ifndef HOMEWIND_WEB_ASSETS_H
#define HOMEWIND_WEB_ASSETS_H

{chr(10).join(all_headers)}

#endif // HOMEWIND_WEB_ASSETS_H
"""
    
    assets_header_file = output_path / 'web_assets.h'
    with open(assets_header_file, 'w', encoding='utf-8') as f:
        f.write(assets_header)
    
    print(f"\n✓ Generated {assets_header_file.name}")
    
    # Generate web_assets_manifest.h (asset lookup table)
    manifest_header = f"""/**
 * @file web_assets_manifest.h
 * @brief Auto-generated WebUI asset manifest
 * @warning DO NOT EDIT - Generated by build_webui.py
 * @date {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
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
{','.join(all_manifests)}
}};

/**
 * @brief Number of assets in manifest
 */
static const size_t WEB_ASSETS_COUNT = {len(assets)};

/**
 * @brief WebUI build hash for cache-busting
 * 
 * This hash is derived from all asset data and changes when any asset changes.
 * Used in HTML as ?v= parameter for app.js and app.css to ensure browsers
 * fetch updated assets after WebUI changes.
 * 
 * Example usage in HTML:
 *   <link rel="stylesheet" href="app.css?v={build_hash}">
 *   <script src="app.js?v={build_hash}" defer></script>
 */
#define HOMEWIND_WEBUI_BUILD_HASH "{build_hash}"

#endif // HOMEWIND_WEB_ASSETS_MANIFEST_H
"""
    
    manifest_header_file = output_path / 'web_assets_manifest.h'
    with open(manifest_header_file, 'w', encoding='utf-8') as f:
        f.write(manifest_header)
    
    print(f"✓ Generated {manifest_header_file.name}")
    
    # Calculate summary
    total_original = 0
    total_minified = 0
    total_embedded = 0
    for asset in assets:
        total_original += asset['original_size']
        total_minified += asset['minified_size']
        total_embedded += asset['final_size']
    
    # Print summary
    print(f"\n=== Build Summary ===")
    print(f"Assets processed: {len(assets)}")
    print(f"Total size (original): {total_original:,} bytes")
    if total_original != total_minified:
        reduction = total_original - total_minified
        reduction_pct = (reduction / total_original) * 100 if total_original > 0 else 0
        print(f"Total size (after comment removal): {total_minified:,} bytes ({reduction:,} bytes, {reduction_pct:.1f}% reduction)")
    print(f"Total size (embedded): ~{total_embedded:,} bytes")
    compression_ratio = (1 - total_embedded / total_minified) * 100 if total_minified > 0 else 0
    print(f"Compression ratio: {compression_ratio:.1f}%")
    if total_original != total_minified:
        total_reduction = total_original - total_embedded
        total_reduction_pct = (total_reduction / total_original) * 100 if total_original > 0 else 0
        print(f"Total reduction (original → embedded): {total_reduction:,} bytes ({total_reduction_pct:.1f}%)")
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

