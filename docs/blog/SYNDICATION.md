# Blog publication and PostgreSQL syndication

The normal MkDocs build generates two RSS 2.0 feeds:

- `https://postgamma.com/blog/feed.xml`: all published posts;
- `https://postgamma.com/blog/postgresql.xml`: published Engineering
  posts, identified by their author.

Both contain rendered full text, canonical article URLs, stable permalink GUIDs,
publication dates, categories, and author names. Static figures and tables stay
in full text; website-only interactive models are replaced with article links.
Drafts are excluded even when a preview build includes drafts. A normal public article uses `draft: false`.

Engineering is the technical feed category. Use this metadata for a technical article:

```yaml
author: Shujie Zhang
categories:
  - Engineering
draft: false
```

All published Engineering articles enter the technical feed automatically; no
extra selection flag is required. Product announcements stay in the all-posts
feed. Before assigning Engineering, review the article for substantive PostgreSQL
content, correct claims, English text, attribution, and the absence of advertising.
Do not assign Engineering merely because an article mentions PostgreSQL.

[Planet PostgreSQL](https://planet.postgresql.org/) is a moderated aggregator.
Read its [policy](https://www.postgresql.org/about/policies/planet-postgresql/)
and [submission instructions](https://planet.postgresql.org/add.html).
The author submits the public technical feed using their PostgreSQL Community
Account. Generating or publishing a feed does not enroll it, and this repository
does not submit requests or send messages to moderators automatically.
Submit only after the selected posts and the feed are accessible on the public
site. Moderator approval is a separate step.

Check feed selection, draft exclusion, full-text URLs, dates and attribution with:

```sh
python3 -m unittest tests.buildsys.test_blog_feed
```
