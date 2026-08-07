# -*- coding: utf-8 -*-
"""
artworks.py — 世界名畫 X3 待機壁紙清單(單一資料來源)

每筆:
  slug          輸出檔名(不含副檔名),同時也是 sleep/_src/ 原圖檔名
  title / artist 標籤文字(清單原文,英文,不翻譯)
  orientation   "portrait"(528x792 直接排版)或 "landscape"(792x528 排版後轉90度)
                依畫作原始比例決定,不是裁切方便決定
  commons_file  Wikimedia Commons 上的精確檔名(用於 Special:FilePath 下載,已逐一搜尋核對)
"""

ARTWORKS = [
    dict(
        slug="mona_lisa",
        title="Mona Lisa",
        artist="Leonardo da Vinci",
        orientation="portrait",
        commons_file="Leonardo da Vinci - Mona Lisa.jpg",
    ),
    dict(
        slug="last_supper",
        title="The Last Supper",
        artist="Leonardo da Vinci",
        orientation="landscape",
        commons_file="Leonardo da Vinci (1452-1519) - The Last Supper (1495-1498).jpg",
        fit=True,  # 13 人橫跨全幅、左右對稱,置中裁會切掉兩端門徒→完整呈現
    ),
    dict(
        slug="birth_of_venus_botticelli",
        title="The Birth of Venus",
        artist="Sandro Botticelli",
        orientation="landscape",
        commons_file="The Birth of Venus (Botticelli) 1.jpg",
    ),
    dict(
        slug="girl_pearl_earring",
        title="Girl with a Pearl Earring",
        artist="Johannes Vermeer",
        orientation="portrait",
        commons_file="Johannes Vermeer - Girl with a Pearl Earring - 670 - Mauritshuis.jpg",
    ),
    dict(
        slug="night_watch",
        title="The Night Watch",
        artist="Rembrandt",
        orientation="landscape",
        commons_file="The Nightwatch by Rembrandt - Rijksmuseum.jpg",
    ),
    dict(
        slug="great_wave",
        title="The Great Wave off Kanagawa",
        artist="Katsushika Hokusai",
        orientation="landscape",
        commons_file="The Great Wave off Kanagawa.jpg",
    ),
    dict(
        slug="sudden_shower_shin_ohashi",
        title="Sudden Shower over Shin-Ōhashi Bridge",
        artist="Utagawa Hiroshige",
        orientation="portrait",
        commons_file="Hiroshige, Sudden shower over Shin-Ōhashi bridge and Atake, 1857.jpg",
    ),
    dict(
        slug="wanderer_sea_of_fog",
        title="Wanderer above the Sea of Fog",
        artist="Caspar David Friedrich",
        orientation="portrait",
        commons_file="Caspar David Friedrich - Wanderer above the Sea of Fog.jpeg",
    ),
    dict(
        slug="gulf_stream",
        title="The Gulf Stream",
        artist="Winslow Homer",
        orientation="landscape",
        # 換掉 The Fighting Temeraire(透納):全解析度 dither 模擬後,透納那種柔霧氛圍的
        # 局部對比本來就低,4 階灰階救不回來。換成明暗對比強烈的同類海難主題。
        commons_file="Winslow Homer - The Gulf Stream - Metropolitan Museum of Art.jpg",
    ),
    dict(
        slug="hay_wain",
        title="The Hay Wain",
        artist="John Constable",
        orientation="landscape",
        commons_file="John Constable - The Hay Wain (1821).jpg",
    ),
    dict(
        slug="red_fuji",
        title="Fine Wind, Clear Morning",
        artist="Katsushika Hokusai",
        orientation="landscape",
        # 換掉 Impression, Sunrise(莫內):畫面全靠「橘色太陽 vs 藍灰霧霾」的色相對比,
        # 兩者亮度幾乎相同,轉灰階後對比幾乎消失,4 階 dither 也生不出本來沒有的對比。
        # 換成跟大浪同系列、明暗對比強烈的浮世繪版畫。
        commons_file="Katsushika Hokusai - Fine Wind, Clear Morning (Gaifū kaisei) - Google Art Project.jpg",
    ),
    dict(
        slug="water_lily_pond",
        title="Water-Lily Pond",
        artist="Claude Monet",
        orientation="landscape",
        commons_file="Claude Monet, The Water-Lily Pond (National Gallery, London).jpg",
        focus=(0.5, 0.4),  # 日本橋在上半部,裁高時焦點上移保住橋(主體)
    ),
    dict(
        slug="starry_night",
        title="The Starry Night",
        artist="Vincent van Gogh",
        orientation="landscape",
        # 原本用 MoMA.jpg 那份實際是遊客在畫前拍的展場照(CC-BY 授權=Flickr 照片的常見訊號,
        # 應提高警覺)。這份是 Wikipedia 條目正文採用的檔案,已下載核對過是乾淨畫作本身。
        commons_file="Van Gogh - Starry Night - Google Art Project.jpg",
    ),
    dict(
        slug="starry_night_rhone",
        title="Starry Night Over the Rhône",
        artist="Vincent van Gogh",
        orientation="landscape",
        commons_file="Starry Night Over the Rhone.jpg",
    ),
    dict(
        slug="cafe_terrace_night",
        title="Café Terrace at Night",
        artist="Vincent van Gogh",
        orientation="portrait",
        # 原本這個檔名實際內容是梵谷的鋼筆素描草稿,不是大家熟悉的彩色油畫。
        # 改用 Yorck Project 版本(已下載核對過是正確的油畫)。
        commons_file="Vincent Willem van Gogh - Cafe Terrace at Night (Yorck).jpg",
    ),
    dict(
        slug="the_kiss",
        title="The Kiss",
        artist="Gustav Klimt",
        orientation="portrait",
        commons_file="Klimt - The Kiss.jpg",
    ),
    dict(
        slug="composition_viii",
        title="Composition VIII",
        artist="Wassily Kandinsky",
        orientation="landscape",
        commons_file="Kandinsky - Composition 8, July 1923.jpg",
    ),
    dict(
        slug="twittering_machine",
        title="Twittering Machine",
        artist="Paul Klee",
        orientation="portrait",
        commons_file="Die Zwitscher-Maschine (Twittering Machine), 1922 - Paul Klee.jpg",
    ),
    dict(
        slug="zodiac",
        title="Zodiac",
        artist="Alphonse Mucha",
        orientation="portrait",
        commons_file="Alphonse Mucha - Zodiac, 1869.jpg",
    ),
    dict(
        slug="birth_of_venus_bouguereau",
        title="The Birth of Venus",
        artist="William-Adolphe Bouguereau",
        orientation="portrait",
        commons_file="William-Adolphe Bouguereau (1825-1905) - The Birth of Venus (1879).jpg",
    ),
    # --- 2026-07-22 第二批:「跟 Great Wave 同類型」——粗黑線條+大塊平塗對比,
    # 不限浮世繪/不限單一畫家,e-ink 4 階 dithering 天生吃得下這種構圖 ---
    dict(
        slug="plum_park_kameido",
        title="Plum Park in Kameido",
        artist="Utagawa Hiroshige",
        orientation="portrait",
        commons_file="De pruimenboomgaard te Kameido-Rijksmuseum RP-P-1956-743.jpeg",
    ),
    dict(
        slug="takiyasha_skeleton",
        title="Takiyasha the Witch and the Skeleton Spectre",
        artist="Utagawa Kuniyoshi",
        orientation="portrait",
        # 原圖是三聯畫(triptych,三張分開的紙),裁到跨兩幅會露出中間那道版面接縫(一條直線)。
        # 2026-07-22 改裁「中間單幅」(巨大骷髏俯視兩武士 = 這幅最經典的畫面),無接縫、直向。
        # _src/*.jpg 已是裁好的中間 panel(x650-1270);別用 fetch_sources.py --force 覆蓋。
        commons_file="Takiyasha the Witch and the Skeleton Spectre, by Utagawa Kuniyoshi.jpg",
    ),
    dict(
        slug="sharaku_otani_oniji",
        title="Ōtani Oniji III as Yakko Edobei",
        artist="Tōshūsai Sharaku",
        orientation="portrait",
        commons_file="三代目大谷鬼次の奴江戸兵衛-Kabuki Actor Ōtani Oniji III as Yakko Edobei in the Play The Colored Reins of a Loving Wife (Koi nyōbō somewake tazuna) MET DP130228.jpg",
    ),
    dict(
        slug="ejiri_suruga",
        title="Ejiri in Suruga Province",
        artist="Katsushika Hokusai",
        orientation="landscape",
        # 原本用的 "...eiri in provincia di suruga...JPG" 是**博物館展框翻拍照**(泛黃、打光不均、有外框)。
        # 2026-07-22 換成乾淨平面掃描,裁掉底部米色紙邊(_src 已裁好,別 --force 覆蓋)。
        commons_file="Ejiri in the Suruga province.jpg",
    ),
    dict(
        slug="dore_dante_inferno",
        title="Dante's Inferno, Canto I",
        artist="Gustave Doré",
        orientation="portrait",
        # 原本用的 "Gustave Dore Inferno1.jpg" 是**書本圖版翻拍**(泛黃、網點濁、底部有書本說明文字)。
        # 2026-07-22 換成乾淨高對比的版畫掃描(Plate 1 系列),裁掉白邊+底部文字(_src 已裁好,別 --force 覆蓋)。
        commons_file="Gustave Doré - Dante Alighieri - Inferno - Plate 1 (I found myself within a forest dark...).jpg",
    ),
    dict(
        slug="peacock_skirt",
        title="The Peacock Skirt",
        artist="Aubrey Beardsley",
        orientation="portrait",
        commons_file="Beardsley-peacockskirt.PNG",
    ),
    # === 世界名畫 50 擴充(2026-07-22,策展補洞:盛期/北方文藝復興、西班牙、巴洛克、
    # 印象派、後印象派、表現主義、美國)。全西方正典、全 PD。方向依實際長寬比;過寬會裁太多
    # 的已換掉(高更《我們從何處來》→《佈道後幻象》;波希三聯畫→布勒哲爾《巴別塔》)。
    # 標 [色彩測] 的靠色相、e-ink 可能扁,已列為 dithering 實測對象。_src 皆已放好。 ===
    dict(slug="creation_of_adam", title="The Creation of Adam", artist="Michelangelo",
         orientation="landscape", commons_file="Michelangelo - Creation of Adam (cropped).jpg",
         fit=True),  # 上帝(右)與亞當(左)分踞兩端,置中裁會切掉亞當→letterbox 完整呈現
    dict(slug="school_of_athens", title="The School of Athens", artist="Raphael",
         orientation="landscape", commons_file="Raffael 058.jpg"),
    dict(slug="arnolfini", title="The Arnolfini Portrait", artist="Jan van Eyck",
         orientation="portrait", commons_file="Van Eyck - Arnolfini Portrait.jpg"),
    dict(slug="hunters_snow", title="The Hunters in the Snow", artist="Pieter Bruegel the Elder",
         orientation="landscape", commons_file="Pieter Bruegel the Elder - Hunters in the Snow (Winter) - Google Art Project.jpg"),
    dict(slug="tower_of_babel", title="The Tower of Babel", artist="Pieter Bruegel the Elder",
         orientation="landscape", commons_file="Pieter Bruegel the Elder - The Tower of Babel (Vienna) - Google Art Project.jpg"),
    dict(slug="durer_melencolia", title="Melencolia I", artist="Albrecht Dürer",
         orientation="portrait", commons_file="Albrecht Dürer - Melencolia I - Google Art Project.jpg"),
    dict(slug="las_meninas", title="Las Meninas", artist="Diego Velázquez",
         orientation="portrait", commons_file="Las Meninas, by Diego Velázquez, from Prado in Google Earth.jpg",
         fit=True),  # 群像橫跨全幅(左=畫家自畫像),置中裁會切掉兩側→完整呈現
    dict(slug="caravaggio_judith", title="Judith Beheading Holofernes", artist="Caravaggio",
         orientation="landscape", commons_file="Judith Beheading Holofernes-Caravaggio (c.1598-9).jpg"),
    dict(slug="milkmaid", title="The Milkmaid", artist="Johannes Vermeer",
         orientation="portrait", commons_file="Johannes Vermeer - Het melkmeisje - Google Art Project.jpg",
         focus=(0.6, 0.5)),  # 女僕在畫面右側,裁寬時焦點右移保住人物
    dict(slug="goya_third_may", title="The Third of May 1808", artist="Francisco Goya",
         orientation="landscape", commons_file="El Tres de Mayo, by Francisco de Goya, from Prado in Google Earth.jpg"),
    dict(slug="manet_dejeuner", title="Le Déjeuner sur l'herbe", artist="Édouard Manet",
         orientation="landscape", commons_file="Édouard Manet - Le Déjeuner sur l'herbe.jpg"),
    dict(slug="degas_star", title="The Star", artist="Edgar Degas",
         orientation="portrait", commons_file="Edgar Degas - The Star - Google Art Project.jpg"),
    # 雷諾瓦《煎餅磨坊》實測 e-ink 失敗(斑斕光→雜訊),2026-07-22 換卡耶博特(灰調雨天、明暗強)。
    dict(slug="caillebotte_rainy", title="Paris Street; Rainy Day", artist="Gustave Caillebotte",
         orientation="landscape", commons_file="Gustave Caillebotte - Paris Street; Rainy Day - Google Art Project.jpg"),
    dict(slug="cezanne_cardplayers", title="The Card Players", artist="Paul Cézanne",
         orientation="landscape", commons_file="Paul Cézanne, Les joueurs de carte (1892-95).jpg"),
    # 秀拉《大碗島》實測 e-ink 失敗(點描+粉色→灰糊),2026-07-22 換沙金《X 夫人》(黑禮服 vs 蒼白膚,對比極強)。
    # _src 用無框版("Sargent MadameX.jpeg";MET DT278076 那版含金框需裁,故用此版)。
    dict(slug="sargent_madame_x", title="Madame X", artist="John Singer Sargent",
         orientation="portrait", commons_file="Sargent MadameX.jpeg",
         focus=(0.5, 0.42)),  # 站姿全身,頭在上方,裁高時焦點上移保住頭與上半身
    dict(slug="gauguin_vision", title="Vision after the Sermon", artist="Paul Gauguin",
         orientation="landscape", commons_file="La vision après le sermon (Paul Gauguin).jpg"),  # [色彩測]
    dict(slug="lautrec_moulin", title="Moulin Rouge: La Goulue", artist="Henri de Toulouse-Lautrec",
         orientation="portrait", commons_file="Moulin Rouge – La Goulue, by Henri de Toulouse-Lautrec.jpg"),
    dict(slug="munch_scream", title="The Scream", artist="Edvard Munch",
         orientation="portrait", commons_file="The Scream.jpg"),
    dict(slug="modigliani", title="Portrait of Jeanne Hébuterne", artist="Amedeo Modigliani",
         orientation="portrait", commons_file="Amedeo Modigliani - Portrait of Jeanne Hébuterne - Google Art Project.jpg"),
    dict(slug="rousseau_gypsy", title="The Sleeping Gypsy", artist="Henri Rousseau",
         orientation="landscape", commons_file="La Bohémienne endormie.jpg"),
    dict(slug="millais_ophelia", title="Ophelia", artist="John Everett Millais",
         orientation="landscape", commons_file="John Everett Millais - Ophelia - Google Art Project.jpg"),
    dict(slug="bocklin_isle", title="Isle of the Dead", artist="Arnold Böcklin",
         orientation="landscape", commons_file="Arnold Böcklin - Die Toteninsel I (Basel, Kunstmuseum).jpg"),
    dict(slug="whistlers_mother", title="Whistler's Mother", artist="James McNeill Whistler",
         orientation="landscape", commons_file="Whistlers Mother high res.jpg",
         fit=True),  # 精心的上下構圖平衡(牆上畫框、腳踏),裁高會破壞→完整呈現
    dict(slug="american_gothic", title="American Gothic", artist="Grant Wood",
         orientation="portrait", commons_file="Grant Wood - American Gothic - Google Art Project.jpg"),
]
