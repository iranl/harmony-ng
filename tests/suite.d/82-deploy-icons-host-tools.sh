# Host tools: build/deploy_hub.py --dry-run names the packed icon archive it would upload, not
# its temporary copy (post-merge check of #258)
python3 "$SCRIPT_DIR/test_deploy_icon_dry_run.py"
