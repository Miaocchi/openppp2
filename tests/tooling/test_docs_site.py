import tempfile
import unittest
from pathlib import Path

from tools.docs_site import DocsSite, canonical, check_links, heading_ids


class DocsSiteTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.files = {
            'docs/README.md': '# Tasks\n[Chinese](README_CN.md)\n',
            'docs/README_CN.md': '# 任务\n[English](README.md)\n',
            'docs/guides/README.md': '# Guides\n',
            'docs/guides/README_CN.md': '# 指南\n',
            'docs/guides/ROUTING_AND_DNS.md': '# Routing and DNS\n## A title\n',
            'docs/guides/ROUTING_AND_DNS_CN.md': '# 分流和 DNS\n## 中文标题\n',
            'docs/guides/PROXY_MODE.md': '# Proxy mode\n',
            'docs/guides/PLATFORMS_CN.md': '# 平台说明\n',
            'docs/guides/image.svg': '<svg/>',
            'docs/guides/INTERNAL.md': '# Internal guide\n',
            'docs/reference/PROJECT_INTERFACE_MAP.md': '# Interface map\n',
            'docs/design/README.md': '# Design\n',
            'docs/archive/audits/OLD.md': '# Old audit\n',
            'README.md': '# Repository\n',
        }
        for relative, text in self.files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.site = DocsSite(self.root)
        self.site.discover([Path(p) for p in self.files if p.startswith('docs/')])

    def test_navigation_uses_handbook_order_and_localized_titles(self):
        self.assertEqual([
            {'Home': 'index.md'},
            {'Getting started': [{'平台说明': 'guides/PLATFORMS.md'}]},
            {'Guides': [
                {'Guides': 'guides/index.md'},
                {'Proxy mode': 'guides/PROXY_MODE.md'},
                {'Routing and DNS': 'guides/ROUTING_AND_DNS.md'},
            ]},
        ], self.site.navigation('en'))
        self.assertEqual({'使用指南': [
            {'指南': 'guides/index.md'},
            {'Proxy mode': 'guides/PROXY_MODE.md'},
            {'分流和 DNS': 'guides/ROUTING_AND_DNS.md'},
        ]}, self.site.navigation('zh')[-1])

    def test_pairs_and_homepages_share_paths(self):
        self.assertEqual(Path('index.md'), canonical(Path('README_CN.md')))
        self.assertEqual(Path('guides/index.md'), canonical(Path('guides/README.md')))
        page = Path('guides/ROUTING_AND_DNS.md')
        self.assertEqual(Path('guides/ROUTING_AND_DNS_CN.md'), self.site.selected(page, 'zh'))

    def test_unpaired_documents_preserve_original_language(self):
        english = self.site.render(Path('guides/PROXY_MODE.md'), 'zh')
        self.assertIn('仅有英文版本', english)
        self.assertIn('# Proxy mode', english)
        chinese = self.site.render(Path('guides/PLATFORMS.md'), 'en')
        self.assertIn('available only in Chinese', chinese)
        self.assertIn('# 平台说明', chinese)

    def test_cross_language_and_directory_links(self):
        source = Path('guides/ROUTING_AND_DNS.md')
        self.assertEqual('/openppp2/zh/guides/ROUTING_AND_DNS/#中文标题', self.site.destination('ROUTING_AND_DNS_CN.md#中文标题', source, 'en'))
        self.assertEqual('/openppp2/guides/', self.site.destination('README.md', source, 'en'))
        self.assertEqual('/openppp2/', self.site.destination('../README.md', source, 'en'))
        self.assertEqual('/openppp2/zh/guides/PROXY_MODE/', self.site.destination('PROXY_MODE.md', source, 'zh'))
        label = '[English](ROUTING_AND_DNS.md) · [分流和 DNS](ROUTING_AND_DNS.md)'
        self.assertEqual('[English](/openppp2/guides/ROUTING_AND_DNS/) · [分流和 DNS](/openppp2/zh/guides/ROUTING_AND_DNS/)',
                         self.site.rewrite(label, Path('guides/README_CN.md'), 'zh'))
        with self.assertRaisesRegex(ValueError, 'Missing documentation link'):
            self.site.destination('MISSING.md', source, 'en')
        self.assertEqual('/openppp2/guides/ROUTING_AND_DNS/', self.site.destination('ROUTING_AND_DNS.md', Path('guides/ROUTING_AND_DNS_CN.md'), 'zh'))

    def test_repository_links(self):
        source = Path('guides/ROUTING_AND_DNS.md')
        self.assertEqual('https://github.com/Miaocchi/openppp2/blob/main/docs/design/README.md#details',
                         self.site.destination('../design/README.md#details', source, 'zh'))
        self.assertEqual('https://github.com/Miaocchi/openppp2/blob/main/README.md', self.site.destination('../../README.md', source, 'en'))
        self.assertEqual('https://example.com/a?q=1#x', self.site.destination('https://example.com/a?q=1#x', source, 'en'))

    def test_rewrite_preserves_examples_and_handles_reference_and_html_links(self):
        text = ('[Routing](ROUTING_AND_DNS_CN.md#中文标题)\n'
                '`[Example](ROUTING_AND_DNS_CN.md)`\n'
                '````markdown\n[Example](ROUTING_AND_DNS_CN.md)\n```\n````\n'
                '    [Indented](ROUTING_AND_DNS_CN.md)\n'
                '[ref]: ROUTING_AND_DNS_CN.md "Title"\n'
                '<img src="image.svg">\n')
        result = self.site.rewrite(text, Path('guides/ROUTING_AND_DNS.md'), 'en')
        self.assertIn('[Routing](/openppp2/zh/guides/ROUTING_AND_DNS/#中文标题)', result)
        self.assertIn('`[Example](ROUTING_AND_DNS_CN.md)`', result)
        self.assertIn('````markdown\n[Example](ROUTING_AND_DNS_CN.md)\n```\n````\n', result)
        self.assertIn('    [Indented](ROUTING_AND_DNS_CN.md)', result)
        self.assertIn('[ref]: /openppp2/zh/guides/ROUTING_AND_DNS/ "Title"', result)
        self.assertIn('<img src="https://github.com/Miaocchi/openppp2/blob/main/docs/guides/image.svg">', result)

    def test_unicode_heading_ids_and_duplicate_fragments(self):
        result = heading_ids('# 中文标题\n## 中文标题\n## `ppp` 参数！\n')
        self.assertIn('{#中文标题}', result)
        self.assertIn('{#中文标题-1}', result)
        self.assertIn('{#ppp-参数}', result)
        self.assertEqual('## Already {#custom}\n', heading_ids('## Already {#custom}\n'))

    def test_repeat_preparation_removes_stale_inputs_and_output(self):
        import shutil
        from tools.docs_site import ROOT
        shutil.copy2(ROOT / 'zensical.toml', self.root / 'zensical.toml')
        shutil.copytree(ROOT / 'tools/docs_theme', self.root / 'tools/docs_theme')
        self.site.prepare()
        stale_input = self.site.work / 'en/docs/REMOVED.md'
        stale_input.write_text('# Removed')
        stale_output = self.site.work / 'site/stale/index.html'
        stale_output.parent.mkdir(parents=True)
        stale_output.write_text('stale')
        self.site.prepare()
        self.assertFalse(stale_input.exists())
        self.assertFalse(stale_output.exists())
        for language in ('en', 'zh'):
            pages = self.site.work / language / 'docs'
            self.assertTrue((pages / 'guides/ROUTING_AND_DNS.md').is_file())
            for repo_only in ('design/README.md', 'archive/audits/OLD.md',
                              'guides/INTERNAL.md', 'guides/image.svg',
                              'reference/PROJECT_INTERFACE_MAP.md'):
                self.assertFalse((pages / repo_only).exists(), repo_only)

    def test_generated_link_checker_covers_base_paths_assets_and_fragments(self):
        site = self.root / 'site'
        (site / 'zh').mkdir(parents=True)
        (site / 'index.html').write_text('<a href="/openppp2/zh/#中文">中文</a><img src="a.svg">')
        (site / 'a.svg').write_text('<svg/>')
        (site / 'zh/index.html').write_text('<h1 id="中文">中文</h1><a href="../">English</a>')
        self.assertEqual([], check_links(site, '/openppp2/'))
        (site / 'index.html').write_text('<a href="/zh/">wrong base</a><a href="zh/#missing">bad anchor</a><img src="missing.svg">')
        errors = check_links(site, '/openppp2/')
        self.assertEqual(3, len(errors))
        self.assertTrue(any('missing anchor' in error for error in errors))


if __name__ == '__main__':
    unittest.main()
