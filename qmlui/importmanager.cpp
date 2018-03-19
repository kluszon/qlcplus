/*
  Q Light Controller Plus
  importmanager.cpp

  Copyright (c) Massimo Callegari

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QXmlStreamReader>
#include <QQmlContext>

#include "importmanager.h"
#include "treemodelitem.h"
#include "fixturemanager.h"
#include "functionmanager.h"

#include "qlcfixturedefcache.h"
#include "audioplugincache.h"
#include "rgbscriptscache.h"
#include "qlcfixturemode.h"
#include "qlcfixturedef.h"
#include "collection.h"
#include "qlcconfig.h"
#include "qlcfile.h"
#include "scene.h"
#include "doc.h"
#include "app.h"

ImportManager::ImportManager(QQuickView *view, Doc *doc, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_doc(doc)
    , m_fixtureTree(NULL)
    , m_functionTree(NULL)
{
    m_importDoc = new Doc(this);

    // delete the default cache to use the one in m_doc
    delete m_importDoc->fixtureDefCache();

    // share the original Doc fixture cache
    m_importDoc->setFixtureDefinitionCache(m_doc->fixtureDefCache());

    /* Load channel modifiers templates */
    //m_importDoc->modifiersCache()->load(QLCModifiersCache::systemTemplateDirectory(), true);
    //m_importDoc->modifiersCache()->load(QLCModifiersCache::userTemplateDirectory());

    /* Load RGB scripts */
    //m_importDoc->rgbScriptsCache()->load(RGBScriptsCache::systemScriptsDirectory());
    //m_importDoc->rgbScriptsCache()->load(RGBScriptsCache::userScriptsDirectory());

    m_view->rootContext()->setContextProperty("importManager", this);
}

ImportManager::~ImportManager()
{
    /* invalidate the fixture cache reference since
     * this is shared with the original Doc */
    m_importDoc->setFixtureDefinitionCache(NULL);
    delete m_importDoc;
}

bool ImportManager::loadWorkspace(const QString &fileName)
{
    QString localFilename =  fileName;
    if (localFilename.startsWith("file:"))
        localFilename = QUrl(fileName).toLocalFile();

    bool retval = false;

    if (localFilename.isEmpty() == true)
        return false;

    QXmlStreamReader *doc = QLCFile::getXMLReader(localFilename);
    if (doc == NULL || doc->device() == NULL || doc->hasError())
    {
        qWarning() << Q_FUNC_INFO << "Unable to read from" << localFilename;
        return false;
    }

    while (!doc->atEnd())
    {
        if (doc->readNext() == QXmlStreamReader::DTD)
            break;
    }
    if (doc->hasError())
    {
        QLCFile::releaseXMLReader(doc);
        return QFile::ResourceError;
    }

    m_importDoc->clearContents();

    /* Set the workspace path before loading the new XML. In this way local files
       can be loaded even if the workspace file has been moved */
    m_importDoc->setWorkspacePath(QFileInfo(localFilename).absolutePath());

    if (doc->dtdName() == KXMLQLCWorkspace)
    {
        retval = loadXML(*doc);
    }
    else
    {
        qWarning() << Q_FUNC_INFO << localFilename << "is not a workspace file";
    }

    QLCFile::releaseXMLReader(doc);

    return retval;
}

void ImportManager::apply()
{
    importFixtures();
    importFunctions();
}

bool ImportManager::loadXML(QXmlStreamReader &doc)
{
    if (doc.readNextStartElement() == false)
        return false;

    if (doc.name() != KXMLQLCWorkspace)
    {
        qWarning() << Q_FUNC_INFO << "Workspace node not found";
        return false;
    }

    while (doc.readNextStartElement())
    {
        if (doc.name() == KXMLQLCEngine)
        {
            m_importDoc->loadXML(doc);
        }
/*
        else if (doc.name() == KXMLQLCVirtualConsole)
        {
            m_virtualConsole->loadXML(doc);
        }
*/
        else
        {
            qDebug() << "Skipping tag:" << doc.name().toString();
            doc.skipCurrentElement();
        }
    }

    return true;
}

void ImportManager::setChildrenChecked(TreeModel *tree, bool checked)
{
    if (tree == NULL)
        return;

    for (TreeModelItem *item : tree->items())
    {
        tree->setItemRoleData(item, checked, TreeModel::IsCheckedRole);

        if (item->hasChildren())
            setChildrenChecked(item->children(), checked);
    }
}

void ImportManager::getAvailableFixtureAddress(int channels, int &universe, int &address)
{
    int freeCounter = 0;
    quint32 absAddress = (universe << 9) + address;

    while (1)
    {
        if (m_doc->fixtureForAddress(absAddress) == Fixture::invalidId())
            freeCounter++;
        else
            freeCounter = 0;

        if (freeCounter == channels)
        {
            universe = (absAddress >> 9);
            address = absAddress - (universe * 512) - (channels - 1);
            return;
        }

        absAddress++;
    }
}

void ImportManager::importFixtures()
{
    for (quint32 importID : m_fixtureIDList)
    {
        bool matchFound = false;
        Fixture *importFixture = m_importDoc->fixture(importID);

        /* Check if a Fixture with the same name already exists in m_doc.
         * If it does, check also if the ID needs to be remapped */
        for (Fixture *docFixture : m_doc->fixtures())
        {
            if (docFixture->name() == importFixture->name())
            {
                qDebug() << "Match found. Import ID:" << importID << docFixture->id();
                if (docFixture->id() != importID)
                {
                    qDebug() << "Fixture" << importFixture->name() << "with ID" << importID
                             << "will be remapped to ID" << docFixture->id();
                }

                m_fixtureIDRemap[importID] = docFixture->id();
                matchFound = true;
                break;
            }
        }

        /* if no match is found, it means a new Fixture needs to be created
         * in m_doc, which implies finding an available address and ID remapping */
        if (matchFound == false)
        {
            int uniIdx = 0;
            int address = 0;

            QLCFixtureDef *importDef = importFixture->fixtureDef();
            QLCFixtureMode *importMode = importFixture->fixtureMode();
            QLCFixtureDef *fxiDef = m_doc->fixtureDefCache()->fixtureDef(importDef->manufacturer(), importDef->model());
            QLCFixtureMode *fxiMode = NULL;

            if (fxiDef != NULL && importMode != NULL)
                fxiMode = fxiDef->mode(importMode->name());

            Fixture *fxi = new Fixture(m_doc);
            fxi->setName(importFixture->name());

            getAvailableFixtureAddress(importFixture->channels(), uniIdx, address);
            fxi->setUniverse(uniIdx);
            fxi->setAddress(address);

            if (fxiDef == NULL && fxiMode == NULL)
            {
                if (importDef->model() == "Generic Dimmer")
                {
                    fxiDef = fxi->genericDimmerDef(importFixture->channels());
                    fxiMode = fxi->genericDimmerMode(fxiDef, importFixture->channels());
                }
                else
                {
                    qWarning() << "FIXME: Something really bad happened";
                    delete fxi;
                    continue;
                }
            }

            fxi->setFixtureDefinition(fxiDef, fxiMode);

            if (m_doc->addFixture(fxi) == true)
                m_fixtureIDRemap[importID] = fxi->id();
            else
                qWarning() << "ERROR: Failed to add fixture" << importFixture->name()
                         << "to universe" << uniIdx << "@address" << address;
        }
    }
}

void ImportManager::importFunctions()
{
    for (quint32 importID : m_functionIDList)
    {
        Function *importFunction = m_importDoc->function(importID);

        // 1. Create a copy of the original Function. This will always create a new ID
        Function *docFunction = importFunction->createCopy(m_doc, true);
        m_functionIDRemap[importID] = docFunction->id();

        // 2. Check Fixture/Function remapping depending on the Function type
        switch (docFunction->type())
        {
            case Function::SceneType:
            {
                Scene *scene = qobject_cast<Scene *>(docFunction);
                // create a copy of the existing values
                QList<SceneValue> sceneValues = scene->values();
                // point of no return. Delete all values
                scene->clear();
                // remap values against existing/remapped fixtures
                for (SceneValue scv : sceneValues)
                {
                    // add a value only if it is present in the remapping map,
                    // otherwise it means the Fixture disappeared
                    if (m_fixtureIDRemap.contains(scv.fxi))
                    {
                        scv.fxi = m_fixtureIDRemap[scv.fxi];
                        scene->setValue(scv);
                    }
                }
            }
            break;
            case Function::CollectionType:
            {
                Collection *collection = qobject_cast<Collection *>(docFunction);
                // create a copy of the existing function IDs
                QList<quint32> funcList = collection->functions();

                // point of no return. Empty the Collection
                for (quint32 id : funcList)
                    collection->removeFunction(id);

                // Add only the functions that have been imported,
                // with their remapped IDs
                for (quint32 id : funcList)
                {
                    if (m_functionIDRemap.contains(id))
                        collection->addFunction(m_functionIDRemap[id]);
                }
            }
            break;
            default:
                qDebug() << "FIXME: Unhandled Function type" << docFunction->type();
            break;
        }
    }
}

void ImportManager::slotFixtureTreeDataChanged(TreeModelItem *item, int role, const QVariant &value)
{
    qDebug() << "Fixture tree data changed" << value.toInt() << "data" << item->data() << "value" << value;

    if (role != TreeModel::IsCheckedRole)
        return;

    bool checked = value.toBool();

    if (item->hasChildren())
        setChildrenChecked(item->children(), checked);

    QVariantList itemData = item->data();
    // itemData must be "classRef" << "type" << "id" << "subid" << "chIdx";
    if (itemData.count() != 5 || itemData.at(1).toInt() != App::FixtureDragItem)
        return;

    //QString type = itemData.at(1).toString();
    quint32 fixtureID = itemData.at(2).toUInt();
    if (checked)
    {
        if (m_fixtureIDList.contains(fixtureID) == false)
            m_fixtureIDList.append(fixtureID);
    }
    else
    {
        m_fixtureIDList.removeOne(fixtureID);
    }

    qDebug() << "Selected fixtures:" << m_fixtureIDList.count();
}

void ImportManager::slotFunctionTreeDataChanged(TreeModelItem *item, int role, const QVariant &value)
{
    qDebug() << "Function tree data changed" << value.toInt() << "data" << item->data();

    if (role != TreeModel::IsCheckedRole)
        return;

    bool checked = value.toBool();

    if (item->hasChildren())
        setChildrenChecked(item->children(), checked);

    QVariantList itemData = item->data();
    if (itemData.isEmpty())
        return;

    QVariant cRef = item->data().first();
    if (cRef.canConvert<Function *>())
    {
        Function *func = cRef.value<Function *>();
        if (checked)
        {
            if (m_functionIDList.contains(func->id()) == false)
                m_functionIDList.append(func->id());
        }
        else
        {
            m_functionIDList.removeOne(func->id());
        }
    }

    qDebug() << "Selected functions:" << m_functionIDList.count();
}

QVariant ImportManager::groupsTreeModel()
{
    if (m_fixtureTree == NULL)
    {
        m_fixtureTree = new TreeModel(this);
        QQmlEngine::setObjectOwnership(m_fixtureTree, QQmlEngine::CppOwnership);
        QStringList treeColumns;
        treeColumns << "classRef" << "type" << "id" << "subid" << "chIdx";
        m_fixtureTree->setColumnNames(treeColumns);
        m_fixtureTree->enableSorting(false);
        m_fixtureTree->setCheckable(true);

        FixtureManager::updateGroupsTree(m_importDoc, m_fixtureTree, m_fixtureSearchFilter, false);

        connect(m_fixtureTree, SIGNAL(roleChanged(TreeModelItem*,int,const QVariant&)),
                this, SLOT(slotFixtureTreeDataChanged(TreeModelItem*,int,const QVariant&)));
    }

    return QVariant::fromValue(m_fixtureTree);
}

void ImportManager::updateFunctionsTree()
{
    m_functionTree->clear();

    for(Function *func : m_importDoc->functions()) // C++11
    {
        if (func == NULL || func->isVisible() == false)
            return;

        bool expandAll = m_functionSearchFilter.length() >= SEARCH_MIN_CHARS;

        QQmlEngine::setObjectOwnership(func, QQmlEngine::CppOwnership);

        if (m_functionSearchFilter.length() < SEARCH_MIN_CHARS || func->name().toLower().contains(m_functionSearchFilter))
        {
            QVariantList params;
            params.append(QVariant::fromValue(func));
            QString fPath = func->path(true).replace("/", TreeModel::separator());
            m_functionTree->addItem(func->name(), params, fPath, expandAll ? TreeModel::Expanded : 0);
        }
    }
}

QVariant ImportManager::functionsTreeModel()
{
    if (m_functionTree == NULL)
    {
        m_functionTree = new TreeModel(this);
        QQmlEngine::setObjectOwnership(m_fixtureTree, QQmlEngine::CppOwnership);
        QStringList treeColumns;
        treeColumns << "classRef";
        m_functionTree->setColumnNames(treeColumns);
        m_functionTree->enableSorting(false);
        m_functionTree->setCheckable(true);

        updateFunctionsTree();

        connect(m_functionTree, SIGNAL(roleChanged(TreeModelItem*,int,const QVariant&)),
                this, SLOT(slotFunctionTreeDataChanged(TreeModelItem*,int,const QVariant&)));
    }

    return QVariant::fromValue(m_functionTree);
}

QString ImportManager::fixtureSearchFilter() const
{
    return m_fixtureSearchFilter;
}

void ImportManager::setFixtureSearchFilter(QString searchFilter)
{
    if (m_fixtureSearchFilter == searchFilter)
        return;

    int currLen = m_fixtureSearchFilter.length();

    m_fixtureSearchFilter = searchFilter;

    if (searchFilter.length() >= SEARCH_MIN_CHARS ||
        (currLen >= SEARCH_MIN_CHARS && searchFilter.length() < SEARCH_MIN_CHARS))
    {
        FixtureManager::updateGroupsTree(m_importDoc, m_fixtureTree, m_fixtureSearchFilter, false);
        emit groupsTreeModelChanged();
    }

    emit fixtureSearchFilterChanged();
}

QString ImportManager::functionSearchFilter() const
{
    return m_functionSearchFilter;
}

void ImportManager::setFunctionSearchFilter(QString searchFilter)
{
    if (m_functionSearchFilter == searchFilter)
        return;

    int currLen = m_functionSearchFilter.length();

    m_functionSearchFilter = searchFilter;

    if (searchFilter.length() >= SEARCH_MIN_CHARS ||
        (currLen >= SEARCH_MIN_CHARS && searchFilter.length() < SEARCH_MIN_CHARS))
    {
        updateFunctionsTree();
        emit functionsTreeModelChanged();
    }

    emit functionSearchFilterChanged();
}